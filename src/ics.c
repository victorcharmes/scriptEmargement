#define _GNU_SOURCE
#include "ics.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *type_nom(type_seance t)
{
    switch (t) {
    case SEANCE_TD: return "TD";
    case SEANCE_TP: return "TP";
    case SEANCE_CM: return "CM";
    case SEANCE_PARTIEL: return "Partiel";
    default: return "autre";
    }
}

void liste_liberer(liste_seances *l)
{
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static int liste_ajouter(liste_seances *l, const seance *s)
{
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 64;
        seance *nv = realloc(l->v, nc * sizeof *nv);
        if (!nv) return -1;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = *s;
    return 0;
}

/* Copie bornée qui garantit le '\0' final */
static void copier(char *dst, size_t taille, const char *src)
{
    size_t n = strlen(src);
    if (n >= taille) n = taille - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void rogner(char *s)
{
    char *d = s;
    while (*d && isspace((unsigned char)*d)) d++;
    if (d != s) memmove(s, d, strlen(d) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

/* Enlève les échappements iCalendar : \n \N \; \, \\ */
static void desechapper(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            *w++ = (*r == 'n' || *r == 'N') ? '\n' : *r;
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* "20261001T054500Z" (UTC) ou "20261001T074500" (heure locale / TZID).
   Le fuseau du processus doit être Europe/Paris (voir main). */
static int lire_date(const char *val, int utc, time_t *out)
{
    struct tm tm = {0};
    int an, mo, j, h = 0, mi = 0, se = 0;
    if (sscanf(val, "%4d%2d%2dT%2d%2d%2d", &an, &mo, &j, &h, &mi, &se) < 3)
        return -1;
    tm.tm_year = an - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = j;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    tm.tm_isdst = -1;
    if (utc || strchr(val, 'Z'))
        *out = timegm(&tm);
    else
        *out = mktime(&tm);
    return 0;
}

static type_seance classer(const char *t)
{
    if (strncasecmp(t, "TD", 2) == 0 && !isalpha((unsigned char)t[2])) return SEANCE_TD;
    if (strncasecmp(t, "TP", 2) == 0 && !isalpha((unsigned char)t[2])) return SEANCE_TP;
    if (strncasecmp(t, "Cours", 5) == 0 || strcasecmp(t, "CM") == 0) return SEANCE_CM;
    if (strcasestr(t, "partiel") || strcasestr(t, "examen")) return SEANCE_PARTIEL;
    return SEANCE_AUTRE;
}

/* SUMMARY = "KINX7AD1 - Parallélisme; TD" : le type est le dernier segment */
static void decouper_resume(const char *resume, seance *s)
{
    const char *p = strrchr(resume, ';');
    if (p) {
        copier(s->type_brut, sizeof s->type_brut, p + 1);
        size_t n = (size_t)(p - resume);
        if (n >= sizeof s->matiere) n = sizeof s->matiere - 1;
        memcpy(s->matiere, resume, n);
        s->matiere[n] = '\0';
    } else {
        copier(s->type_brut, sizeof s->type_brut, "");
        copier(s->matiere, sizeof s->matiere, resume);
    }
    rogner(s->type_brut);
    rogner(s->matiere);
    s->type = classer(s->type_brut);
}

/* DESCRIPTION = "EVANS Jules; INX7AD1A51; ...\n\nEvent id: ..."
   Un enseignant = segment avec une espace et sans chiffre.
   On garde le premier trouvé. */
static void extraire_formateur(const char *desc, seance *s)
{
    s->formateur[0] = '\0';
    char ligne[1024];
    size_t n = strcspn(desc, "\n");
    if (n >= sizeof ligne) n = sizeof ligne - 1;
    memcpy(ligne, desc, n);
    ligne[n] = '\0';

    char *save = NULL;
    for (char *seg = strtok_r(ligne, ";", &save); seg; seg = strtok_r(NULL, ";", &save)) {
        rogner(seg);
        if (!*seg || !strchr(seg, ' ')) continue;
        if (strpbrk(seg, "0123456789")) continue;
        copier(s->formateur, sizeof s->formateur, seg);
        return;
    }
}

/* Déplie les lignes iCalendar (une ligne commençant par espace/tab
   continue la précédente) et supprime les \r. */
static char *deplier(const char *texte)
{
    size_t n = strlen(texte);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (texte[i] == '\r') continue;
        if (texte[i] == '\n' && (texte[i + 1] == ' ' || texte[i + 1] == '\t' ||
                                 (texte[i + 1] == '\r' && (texte[i + 2] == ' ' || texte[i + 2] == '\t')))) {
            i++;
            if (texte[i] == '\r') i++;
            continue; /* saute le saut de ligne et l'espace de pliage */
        }
        out[w++] = texte[i];
    }
    out[w] = '\0';
    return out;
}

int ics_parser(const char *texte, liste_seances *out)
{
    char *buf = deplier(texte);
    if (!buf) return -1;

    int dans_event = 0, annule = 0, ok_debut = 0, ok_fin = 0;
    seance cur;
    char resume[512], desc[2048];

    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        if (strcmp(l, "BEGIN:VEVENT") == 0) {
            dans_event = 1;
            annule = ok_debut = ok_fin = 0;
            memset(&cur, 0, sizeof cur);
            resume[0] = desc[0] = '\0';
            continue;
        }
        if (!dans_event) continue;

        if (strcmp(l, "END:VEVENT") == 0) {
            dans_event = 0;
            if (annule || !ok_debut || !ok_fin) continue;
            desechapper(resume);
            desechapper(desc);
            decouper_resume(resume, &cur);
            extraire_formateur(desc, &cur);
            if (liste_ajouter(out, &cur) != 0) { free(buf); return -1; }
            continue;
        }

        /* NOM;PARAMS:VALEUR */
        char *deux_points = strchr(l, ':');
        if (!deux_points) continue;
        *deux_points = '\0';
        const char *nom = l, *val = deux_points + 1;
        int utc = 0;

        if (strncmp(nom, "DTSTART", 7) == 0) {
            ok_debut = lire_date(val, utc, &cur.debut) == 0;
        } else if (strncmp(nom, "DTEND", 5) == 0) {
            ok_fin = lire_date(val, utc, &cur.fin) == 0;
        } else if (strncmp(nom, "SUMMARY", 7) == 0) {
            copier(resume, sizeof resume, val);
        } else if (strncmp(nom, "DESCRIPTION", 11) == 0) {
            copier(desc, sizeof desc, val);
        } else if (strcmp(nom, "STATUS") == 0) {
            annule = strcasecmp(val, "CANCELLED") == 0;
        }
    }
    free(buf);
    return 0;
}
