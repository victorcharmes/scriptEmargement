/*
 * emargement — remplit la feuille d'émargement d'alternance (.ods)
 * à partir de l'emploi du temps CELCAT (flux iCalendar).
 *
 * Règles :
 *  - seules les séances CM (« Cours »), TD, TP et partiels/examens comptent ;
 *  - matin = séance qui commence avant 12h30, après-midi sinon ;
 *  - formateur d'une demi-journée : celui du TD, sinon TP, sinon CM, sinon partiel
 *    (si la séance retenue n'a pas d'enseignant renseigné, le nom reste vide) ;
 *  - les séances qui se chevauchent (ex. un partiel réparti sur plusieurs salles)
 *    sont fusionnées, la durée n'est donc jamais comptée deux fois.
 */
#define _GNU_SOURCE
#include <curl/curl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ics.h"
#include "ods.h"

#define NB_LIGNES 23          /* lignes disponibles sur la feuille       */
#define MIDI_MINUTES (12 * 60 + 30) /* limite matin / après-midi        */
#define MAX_CRENEAUX 8

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char url[1024];
    char fichier[512];
    char nom[128];
    char modele[512];
    char sortie[512];
    char dossier[512];
    int annee, mois; /* 0 = automatique */
    int sans_pdf;
} config;

static void lire_conf(const char *chemin, config *c, int obligatoire)
{
    FILE *f = fopen(chemin, "r");
    if (!f) {
        if (obligatoire) { perror(chemin); exit(1); }
        return;
    }
    char ligne[1200];
    while (fgets(ligne, sizeof ligne, f)) {
        ligne[strcspn(ligne, "\r\n")] = '\0';
        if (ligne[0] == '#' || !strchr(ligne, '=')) continue;
        char *eq = strchr(ligne, '=');
        *eq = '\0';
        const char *k = ligne, *v = eq + 1;
        while (*v == ' ') v++;
        if (!strcmp(k, "url")) snprintf(c->url, sizeof c->url, "%s", v);
        else if (!strcmp(k, "nom")) snprintf(c->nom, sizeof c->nom, "%s", v);
        else if (!strcmp(k, "modele")) snprintf(c->modele, sizeof c->modele, "%s", v);
        else if (!strcmp(k, "dossier")) snprintf(c->dossier, sizeof c->dossier, "%s", v);
    }
    fclose(f);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage : %s [options]\n"
        "  -u URL        URL du flux .ics CELCAT\n"
        "  -f FICHIER    fichier .ics local (au lieu de l'URL)\n"
        "  -m AAAA-MM    mois à générer (défaut : mois en cours, ou le suivant\n"
        "                si on est après le 20)\n"
        "  -n NOM        nom du stagiaire\n"
        "  -t MODELE     modèle .ods (défaut : modele_emargement.ods)\n"
        "  -o SORTIE     fichier produit (défaut : emargement_AAAA-MM.ods)\n"
        "  -d DOSSIER    dossier où écrire le fichier par défaut (défaut : dossier courant)\n"
        "  -c CONF       fichier de configuration (défaut : emargement.conf)\n"
        "  -P            ne pas générer le PDF (seulement le .ods)\n",
        prog);
}

/* ------------------------------------------------------------------ */
/* Lecture du flux                                                     */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t n; } memoire;

static size_t ecrire_cb(void *data, size_t taille, size_t nb, void *u)
{
    memoire *m = u;
    size_t n = taille * nb;
    char *np = realloc(m->p, m->n + n + 1);
    if (!np) return 0;
    m->p = np;
    memcpy(m->p + m->n, data, n);
    m->n += n;
    m->p[m->n] = '\0';
    return n;
}

static char *telecharger(const char *url)
{
    memoire m = {0};
    CURL *h = curl_easy_init();
    if (!h) return NULL;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "emargement/1.0");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, ecrire_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &m);
    CURLcode rc = curl_easy_perform(h);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
        fprintf(stderr, "Téléchargement impossible : %s\n", curl_easy_strerror(rc));
        free(m.p);
        return NULL;
    }
    return m.p;
}

static char *lire_fichier(const char *chemin)
{
    FILE *f = fopen(chemin, "rb");
    if (!f) { perror(chemin); return NULL; }
    memoire m = {0};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) ecrire_cb(buf, 1, n, &m);
    fclose(f);
    return m.p;
}

/* ------------------------------------------------------------------ */
/* Calcul de la feuille                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    time_t debut[MAX_CRENEAUX], fin[MAX_CRENEAUX];
    int nb;
    const seance *ref; /* séance qui donne le formateur */
} demi_journee;

typedef struct {
    int jour; /* jour du mois */
    demi_journee matin, aprem;
} journee;

typedef struct {
    char date[32], mh[128], mf[128], ah[128], af[128], duree[24];
} ligne_feuille;

typedef struct {
    ligne_feuille l[NB_LIGNES];
    char nom[128];
    char periode[64];
} feuille;

static int cmp_seance(const void *a, const void *b)
{
    const seance *x = a, *y = b;
    if (x->debut != y->debut) return x->debut < y->debut ? -1 : 1;
    return x->fin < y->fin ? -1 : x->fin > y->fin;
}

/* Ajoute une séance (triées par début) en fusionnant les chevauchements */
static void demi_ajouter(demi_journee *d, const seance *s)
{
    if (d->nb && s->debut <= d->fin[d->nb - 1]) {
        if (s->fin > d->fin[d->nb - 1]) d->fin[d->nb - 1] = s->fin;
    } else if (d->nb < MAX_CRENEAUX) {
        d->debut[d->nb] = s->debut;
        d->fin[d->nb] = s->fin;
        d->nb++;
    }
    /* Priorité formateur : TD > TP > CM > partiel. Le type prime : un TD sans
       enseignant renseigné donne un nom vide, on ne se rabat pas sur le CM.
       À type égal, on préfère une séance dont l'enseignant est connu. */
    if (!d->ref || s->type < d->ref->type ||
        (s->type == d->ref->type && !d->ref->formateur[0] && s->formateur[0]))
        d->ref = s;
}

static long demi_minutes(const demi_journee *d)
{
    long total = 0;
    for (int i = 0; i < d->nb; i++) total += (long)(d->fin[i] - d->debut[i]) / 60;
    return total;
}

static void fmt_heure(time_t t, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%dh%02d", tm.tm_hour, tm.tm_min);
}

static void fmt_creneaux(const demi_journee *d, char *buf, size_t n)
{
    buf[0] = '\0';
    for (int i = 0; i < d->nb; i++) {
        char a[16], b[16], tmp[48];
        fmt_heure(d->debut[i], a, sizeof a);
        fmt_heure(d->fin[i], b, sizeof b);
        snprintf(tmp, sizeof tmp, "%s%s - %s", i ? "\n" : "", a, b);
        strncat(buf, tmp, n - strlen(buf) - 1);
    }
}

static const char *JOURS[] = {"dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam."};
static const char *MOIS[] = {"janvier", "février", "mars", "avril", "mai", "juin", "juillet",
                             "août", "septembre", "octobre", "novembre", "décembre"};

static const char *resoudre(const char *cle, void *ctx)
{
    const feuille *f = ctx;
    if (!strcmp(cle, "NOM")) return f->nom;
    if (!strcmp(cle, "PERIODE")) return f->periode;

    char champ[8] = {0};
    int num = 0;
    if (sscanf(cle, "%7[A-Z]%d", champ, &num) != 2 || num < 1 || num > NB_LIGNES) return NULL;
    const ligne_feuille *l = &f->l[num - 1];
    if (!strcmp(champ, "D")) return l->date;
    if (!strcmp(champ, "MH")) return l->mh;
    if (!strcmp(champ, "MF")) return l->mf;
    if (!strcmp(champ, "AH")) return l->ah;
    if (!strcmp(champ, "AF")) return l->af;
    if (!strcmp(champ, "DT")) return l->duree;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Conversion PDF (LibreOffice en mode headless)                       */
/* ------------------------------------------------------------------ */

#define SOFFICE_WINDOWS "/mnt/c/Program Files/LibreOffice/program/soffice.exe"

/* Convertit `ods` en PDF dans le même dossier ; écrit le chemin produit dans `pdf`.
   Utilise soffice de Linux s'il existe, sinon celui de Windows (WSL). */
static int convertir_pdf(const char *ods, char *pdf, size_t n)
{
    char abs[PATH_MAX], dossier[PATH_MAX], cmd[3 * PATH_MAX];
    if (!realpath(ods, abs)) { perror(ods); return -1; }
    snprintf(dossier, sizeof dossier, "%s", abs);
    *strrchr(dossier, '/') = '\0';

    if (system("command -v soffice >/dev/null 2>&1") == 0) {
        snprintf(cmd, sizeof cmd,
                 "soffice --headless --convert-to pdf --outdir '%s' '%s' >/dev/null 2>&1",
                 dossier, abs);
    } else if (access(SOFFICE_WINDOWS, X_OK) == 0) {
        /* L'exécutable Windows attend des chemins Windows */
        snprintf(cmd, sizeof cmd,
                 "'" SOFFICE_WINDOWS "' --headless --convert-to pdf "
                 "--outdir \"$(wslpath -w '%s')\" \"$(wslpath -w '%s')\" >/dev/null 2>&1",
                 dossier, abs);
    } else {
        fprintf(stderr, "LibreOffice introuvable : PDF non généré "
                        "(sudo apt install libreoffice-calc-nogui).\n");
        return -1;
    }

    snprintf(pdf, n, "%s", ods);
    char *ext = strrchr(pdf, '.');
    if (ext && !strchr(ext, '/')) *ext = '\0';
    strncat(pdf, ".pdf", n - strlen(pdf) - 1);

    remove(pdf);
    if (system(cmd) != 0 || access(pdf, R_OK) != 0) {
        fprintf(stderr, "Échec de la conversion PDF (LibreOffice est-il bloqué ?).\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    setenv("TZ", "Europe/Paris", 1);
    tzset();

    config c = {0};
    snprintf(c.modele, sizeof c.modele, "modele_emargement.ods");

    /* 1er passage : -c éventuel, puis lecture du fichier de conf */
    const char *conf = "emargement.conf";
    int conf_explicite = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "-c")) { conf = argv[i + 1]; conf_explicite = 1; }
    lire_conf(conf, &c, conf_explicite);

    int opt;
    while ((opt = getopt(argc, argv, "u:f:m:n:t:o:d:c:Ph")) != -1) {
        switch (opt) {
        case 'u': snprintf(c.url, sizeof c.url, "%s", optarg); break;
        case 'f': snprintf(c.fichier, sizeof c.fichier, "%s", optarg); break;
        case 'n': snprintf(c.nom, sizeof c.nom, "%s", optarg); break;
        case 't': snprintf(c.modele, sizeof c.modele, "%s", optarg); break;
        case 'o': snprintf(c.sortie, sizeof c.sortie, "%s", optarg); break;
        case 'd': snprintf(c.dossier, sizeof c.dossier, "%s", optarg); break;
        case 'c': break;
        case 'P': c.sans_pdf = 1; break;
        case 'm':
            if (sscanf(optarg, "%d-%d", &c.annee, &c.mois) != 2 || c.mois < 1 || c.mois > 12) {
                fprintf(stderr, "Mois invalide : %s (attendu AAAA-MM)\n", optarg);
                return 1;
            }
            break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    /* Mois cible : en fin de mois (après le 20) on prépare le mois suivant */
    if (!c.mois) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        c.annee = tm.tm_year + 1900;
        c.mois = tm.tm_mon + 1;
        if (tm.tm_mday > 20 && ++c.mois > 12) { c.mois = 1; c.annee++; }
    }
    if (!c.sortie[0])
        snprintf(c.sortie, sizeof c.sortie, "%.400s%s" "emargement_%04d-%02d.ods",
                 c.dossier, c.dossier[0] && c.dossier[strlen(c.dossier) - 1] != '/' ? "/" : "",
                 c.annee, c.mois);

    /* Lecture du flux */
    char *texte = NULL;
    if (c.fichier[0]) {
        texte = lire_fichier(c.fichier);
    } else if (c.url[0]) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        texte = telecharger(c.url);
        curl_global_cleanup();
    } else {
        fprintf(stderr, "Aucune source : indique une URL (-u ou url= dans %s) ou un fichier (-f).\n", conf);
        return 1;
    }
    if (!texte) return 1;

    liste_seances toutes = {0};
    if (ics_parser(texte, &toutes) != 0) { fprintf(stderr, "Flux iCalendar illisible\n"); return 1; }
    free(texte);
    qsort(toutes.v, toutes.n, sizeof *toutes.v, cmp_seance);

    /* Regroupement par jour */
    journee jours[32] = {0};
    int ignores = 0;
    for (size_t i = 0; i < toutes.n; i++) {
        const seance *s = &toutes.v[i];
        struct tm tm;
        localtime_r(&s->debut, &tm);
        if (tm.tm_year + 1900 != c.annee || tm.tm_mon + 1 != c.mois) continue;
        if (s->type == SEANCE_AUTRE) {
            char h[16];
            fmt_heure(s->debut, h, sizeof h);
            fprintf(stderr, "  ignoré : %02d/%02d %s  %s (%s)\n", tm.tm_mday, c.mois, h,
                    s->matiere, s->type_brut[0] ? s->type_brut : "sans type");
            ignores++;
            continue;
        }
        journee *j = &jours[tm.tm_mday];
        j->jour = tm.tm_mday;
        int minutes = tm.tm_hour * 60 + tm.tm_min;
        demi_ajouter(minutes < MIDI_MINUTES ? &j->matin : &j->aprem, s);
    }

    /* Remplissage de la feuille */
    static feuille f;
    snprintf(f.nom, sizeof f.nom, "%s", c.nom);
    snprintf(f.periode, sizeof f.periode, "%s %d", MOIS[c.mois - 1], c.annee);
    f.periode[0] = (char)(f.periode[0] - 'a' + 'A'); /* Majuscule (mois sans accent initial) */

    int n = 0, debordement = 0;
    long total_mois = 0;
    for (int d = 1; d <= 31; d++) {
        journee *j = &jours[d];
        if (!j->jour) continue;
        if (n == NB_LIGNES) { debordement++; continue; }
        ligne_feuille *l = &f.l[n++];

        struct tm tm = {.tm_year = c.annee - 1900, .tm_mon = c.mois - 1, .tm_mday = d, .tm_hour = 12, .tm_isdst = -1};
        mktime(&tm);
        snprintf(l->date, sizeof l->date, "%s %02d/%02d/%04d", JOURS[tm.tm_wday], d, c.mois, c.annee);

        fmt_creneaux(&j->matin, l->mh, sizeof l->mh);
        fmt_creneaux(&j->aprem, l->ah, sizeof l->ah);
        if (j->matin.ref) snprintf(l->mf, sizeof l->mf, "%s", j->matin.ref->formateur);
        if (j->aprem.ref) snprintf(l->af, sizeof l->af, "%s", j->aprem.ref->formateur);

        long m = demi_minutes(&j->matin) + demi_minutes(&j->aprem);
        total_mois += m;
        snprintf(l->duree, sizeof l->duree, "%ldh%02ld", m / 60, m % 60);

        printf("%-17s | %-15s | %-22s | %-15s | %-22s | %s\n", l->date,
               j->matin.nb ? strtok(strdupa(l->mh), "\n") : "", l->mf,
               j->aprem.nb ? strtok(strdupa(l->ah), "\n") : "", l->af, l->duree);
    }
    liste_liberer(&toutes);

    if (debordement)
        fprintf(stderr, "ATTENTION : %d jour(s) de cours en trop pour les %d lignes de la feuille.\n",
                debordement, NB_LIGNES);

    if (ods_remplir(c.modele, c.sortie, resoudre, &f) != 0) return 1;

    printf("\n%d jour(s), %ldh%02ld de cours, %d séance(s) ignorée(s) -> %s\n",
           n, total_mois / 60, total_mois % 60, ignores, c.sortie);

    if (!c.sans_pdf) {
        char pdf[PATH_MAX];
        if (convertir_pdf(c.sortie, pdf, sizeof pdf) == 0) printf("PDF -> %s\n", pdf);
    }
    return 0;
}
