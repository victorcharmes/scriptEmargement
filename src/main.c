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
#include <ctype.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
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
    char formation[128];
    char organisme[128];
    char existence[32];
    char siret[32];
    char modele[PATH_MAX];
    char sortie[PATH_MAX];
    char dossier[PATH_MAX];
    char signature[PATH_MAX];
    int annee, mois; /* 0 = automatique */
    int sans_pdf;
} config;

/* Dossier qui contient `chemin` (sans le '/' final), "." s'il n'y en a pas */
static void dossier_de(const char *chemin, char *out, size_t n)
{
    const char *slash = strrchr(chemin, '/');
    if (!slash) snprintf(out, n, ".");
    else if (slash == chemin) snprintf(out, n, "/");
    else snprintf(out, n, "%.*s", (int)(slash - chemin), chemin);
}

/* `v` relatif est interprété par rapport à `base` ; absolu, il est gardé tel quel */
static void chemin_depuis(const char *base, const char *v, char *out, size_t n)
{
    if (v[0] == '/' || !strcmp(base, ".")) snprintf(out, n, "%s", v);
    else snprintf(out, n, "%s/%s", base, v);
}

static char *rogner(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

/* Lit `cle=valeur` ligne par ligne. Les chemins relatifs (modele, dossier,
   signature) sont relatifs au dossier du fichier de conf. */
static void lire_conf(const char *chemin, config *c, int obligatoire)
{
    FILE *f = fopen(chemin, "r");
    if (!f) {
        if (obligatoire) { perror(chemin); exit(1); }
        return;
    }
    char base[PATH_MAX];
    dossier_de(chemin, base, sizeof base);

    char ligne[1200];
    for (int num = 1; fgets(ligne, sizeof ligne, f); num++) {
        char *l = ligne;
        if (num == 1 && !strncmp(l, "\xEF\xBB\xBF", 3)) l += 3; /* BOM UTF-8 (éditeurs Windows) */
        l = rogner(l);
        if (!*l || *l == '#') continue;
        char *eq = strchr(l, '=');
        if (!eq) {
            fprintf(stderr, "%s, ligne %d ignorée (attendu cle=valeur) : %s\n", chemin, num, l);
            continue;
        }
        *eq = '\0';
        const char *k = rogner(l), *v = rogner(eq + 1);
        if (!strcmp(k, "url")) snprintf(c->url, sizeof c->url, "%s", v);
        else if (!strcmp(k, "nom")) snprintf(c->nom, sizeof c->nom, "%s", v);
        else if (!strcmp(k, "formation")) snprintf(c->formation, sizeof c->formation, "%s", v);
        else if (!strcmp(k, "organisme")) snprintf(c->organisme, sizeof c->organisme, "%s", v);
        else if (!strcmp(k, "existence")) snprintf(c->existence, sizeof c->existence, "%s", v);
        else if (!strcmp(k, "siret")) snprintf(c->siret, sizeof c->siret, "%s", v);
        else if (!strcmp(k, "modele")) chemin_depuis(base, v, c->modele, sizeof c->modele);
        else if (!strcmp(k, "dossier")) chemin_depuis(base, v, c->dossier, sizeof c->dossier);
        else if (!strcmp(k, "signature")) chemin_depuis(base, v, c->signature, sizeof c->signature);
        else fprintf(stderr, "%s, ligne %d : clé inconnue « %s » ignorée\n", chemin, num, k);
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
    int signe_matin, signe_aprem;
} ligne_feuille;

/* Signature du stagiaire, placée dans les colonnes « Signature stagiaire » */
typedef struct {
    const char *chemin;    /* chemin dans l'archive */
    const char *type_mime;
    double largeur, hauteur; /* pouces */
} signature;

typedef struct {
    ligne_feuille l[NB_LIGNES];
    const config *conf;
    char periode[64];
    char annee[16]; /* année scolaire, ex. "2026/2027" */
    signature sig;  /* largeur nulle = pas de signature */
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

/* Dimensions des cases « Signature stagiaire » du modèle, en pouces */
#define LARGEUR_COL_MS 1.2028
#define LARGEUR_COL_AS 1.3139
#define HAUTEUR_LIGNE 0.2917
#define MARGE_SIGNATURE 0.03

/* Rapport largeur / hauteur d'une image SVG (viewBox ou width/height) ou PNG.
   Retourne 0 si le format n'est pas reconnu. */
static double ratio_image(const char *chemin, const char **type_mime)
{
    FILE *fp = fopen(chemin, "rb");
    if (!fp) { perror(chemin); return 0; }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    if (n > 24 && !memcmp(buf, "\x89PNG", 4)) {
        const unsigned char *u = (const unsigned char *)buf;
        double w = (u[16] << 24) | (u[17] << 16) | (u[18] << 8) | u[19];
        double h = (u[20] << 24) | (u[21] << 16) | (u[22] << 8) | u[23];
        *type_mime = "image/png";
        return h > 0 ? w / h : 0;
    }
    if (strstr(buf, "<svg")) {
        double x, y, w = 0, h = 0;
        const char *vb = strstr(buf, "viewBox=\"");
        if (vb && sscanf(vb + 9, "%lf%*[ ,]%lf%*[ ,]%lf%*[ ,]%lf", &x, &y, &w, &h) == 4 && h > 0) {
            *type_mime = "image/svg+xml";
            return w / h;
        }
        const char *a = strstr(buf, " width=\""), *b = strstr(buf, " height=\"");
        if (a && b && sscanf(a + 8, "%lf", &w) == 1 && sscanf(b + 9, "%lf", &h) == 1 && h > 0) {
            *type_mime = "image/svg+xml";
            return w / h;
        }
    }
    return 0;
}

/* Cadre image centré dans la case (XML brut pour ods_remplir) */
static const char *cadre_signature(const signature *s, const char *champ, int num, double largeur_col)
{
    static char xml[1024];
    if (s->largeur <= 0) return NULL;
    double l = s->largeur, h = s->hauteur;
    if (l > largeur_col - 2 * MARGE_SIGNATURE) { /* image trop large : on réduit */
        h *= (largeur_col - 2 * MARGE_SIGNATURE) / l;
        l = largeur_col - 2 * MARGE_SIGNATURE;
    }
    snprintf(xml, sizeof xml, ODS_XML
             "<draw:frame draw:z-index=\"%d\" draw:name=\"Signature %s%02d\" draw:style-name=\"gr1\" "
             "draw:text-style-name=\"P1\" svg:width=\"%.4fin\" svg:height=\"%.4fin\" "
             "svg:x=\"%.4fin\" svg:y=\"%.4fin\"><draw:image xlink:href=\"%s\" xlink:type=\"simple\" "
             "xlink:show=\"embed\" xlink:actuate=\"onLoad\" draw:mime-type=\"%s\"><text:p/></draw:image>"
             "</draw:frame>",
             num * 2 + (champ[0] == 'A'), champ, num, l, h, (largeur_col - l) / 2,
             (HAUTEUR_LIGNE - h) / 2, s->chemin, s->type_mime);
    return xml;
}

static const char *resoudre(const char *cle, void *ctx)
{
    const feuille *f = ctx;
    if (!strcmp(cle, "NOM")) return f->conf->nom;
    if (!strcmp(cle, "FORMATION")) return f->conf->formation;
    if (!strcmp(cle, "ORGANISME")) return f->conf->organisme;
    if (!strcmp(cle, "EXISTENCE")) return f->conf->existence;
    if (!strcmp(cle, "SIRET")) return f->conf->siret;
    if (!strcmp(cle, "PERIODE")) return f->periode;
    if (!strcmp(cle, "ANNEE")) return f->annee;

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
    if (!strcmp(champ, "MS")) return l->signe_matin ? cadre_signature(&f->sig, champ, num, LARGEUR_COL_MS) : NULL;
    if (!strcmp(champ, "AS")) return l->signe_aprem ? cadre_signature(&f->sig, champ, num, LARGEUR_COL_AS) : NULL;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Conversion PDF (LibreOffice en mode headless)                       */
/* ------------------------------------------------------------------ */

#define SOFFICE_WINDOWS "/mnt/c/Program Files/LibreOffice/program/soffice.exe"

/* Lance argv[0] (cherché dans le PATH) sans passer par un shell : les chemins
   peuvent contenir espaces et apostrophes. Si `sortie` est fourni, y recopie
   la sortie standard (sans le saut de ligne final).
   Retourne le code de sortie du programme, 127 s'il est introuvable. */
static int executer(char *const argv[], char *sortie, size_t n)
{
    int tube[2] = {-1, -1};
    if (sortie && pipe(tube) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        dup2(sortie ? tube[1] : nul, STDOUT_FILENO);
        dup2(nul, STDERR_FILENO);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (sortie) {
        close(tube[1]);
        size_t lu = 0;
        ssize_t r;
        while (lu + 1 < n && (r = read(tube[0], sortie + lu, n - 1 - lu)) > 0) lu += (size_t)r;
        sortie[lu] = '\0';
        sortie[strcspn(sortie, "\r\n")] = '\0';
        close(tube[0]);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Convertit `ods` en PDF dans le même dossier ; écrit le chemin produit dans `pdf`.
   Utilise soffice de Linux s'il existe, sinon celui de Windows (WSL). */
static int convertir_pdf(const char *ods, char *pdf, size_t n)
{
    char abs[PATH_MAX], dossier[PATH_MAX];
    if (!realpath(ods, abs)) { perror(ods); return -1; }
    dossier_de(abs, dossier, sizeof dossier);

    snprintf(pdf, n, "%s", ods);
    char *ext = strrchr(pdf, '.');
    if (ext && !strchr(ext, '/')) *ext = '\0';
    strncat(pdf, ".pdf", n - strlen(pdf) - 1);
    remove(pdf);

    char *linux_argv[] = {"soffice", "--headless", "--convert-to", "pdf",
                          "--outdir", dossier, abs, NULL};
    int r = executer(linux_argv, NULL, 0);
    if (r == 127 && access(SOFFICE_WINDOWS, X_OK) == 0) {
        /* L'exécutable Windows attend des chemins Windows */
        char win_dossier[PATH_MAX], win_abs[PATH_MAX];
        char *a1[] = {"wslpath", "-w", dossier, NULL}, *a2[] = {"wslpath", "-w", abs, NULL};
        if (executer(a1, win_dossier, sizeof win_dossier) != 0 ||
            executer(a2, win_abs, sizeof win_abs) != 0) {
            fprintf(stderr, "Conversion des chemins pour Windows impossible (wslpath).\n");
            return -1;
        }
        char *win_argv[] = {SOFFICE_WINDOWS, "--headless", "--convert-to", "pdf",
                            "--outdir", win_dossier, win_abs, NULL};
        r = executer(win_argv, NULL, 0);
    }
    if (r == 127) {
        fprintf(stderr, "LibreOffice introuvable : PDF non généré (voir README.md, Installation).\n");
        return -1;
    }
    if (r != 0 || access(pdf, R_OK) != 0) {
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

    /* Conf, modèle et sortie par défaut : dans le dossier du programme,
       pour pouvoir le lancer depuis n'importe où */
    char exe[PATH_MAX], projet[PATH_MAX], conf_defaut[PATH_MAX];
    ssize_t ne = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (ne > 0) { exe[ne] = '\0'; dossier_de(exe, projet, sizeof projet); }
    else snprintf(projet, sizeof projet, ".");

    static config c;
    chemin_depuis(projet, "modele_emargement.ods", c.modele, sizeof c.modele);
    chemin_depuis(projet, "emargement.conf", conf_defaut, sizeof conf_defaut);
    snprintf(c.dossier, sizeof c.dossier, "%s", projet);

    /* 1er passage : -c éventuel, puis lecture du fichier de conf */
    const char *conf = conf_defaut;
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
    if (!c.sortie[0]) {
        char nom_fichier[32];
        snprintf(nom_fichier, sizeof nom_fichier, "emargement_%04d-%02d.ods", c.annee, c.mois);
        size_t l = strlen(c.dossier);
        if (l && c.dossier[l - 1] == '/') c.dossier[l - 1] = '\0';
        chemin_depuis(c.dossier[0] ? c.dossier : ".", nom_fichier, c.sortie, sizeof c.sortie);
    }
    if (!c.nom[0])
        fprintf(stderr, "Attention : nom= est vide dans %s, la case « Nom du stagiaire » restera vide.\n", conf);

    /* Lecture du flux */
    char *texte = NULL;
    if (c.fichier[0]) {
        texte = lire_fichier(c.fichier);
    } else if (strstr(c.url, "COLLE-ICI")) {
        fprintf(stderr, "Remplace l'URL d'exemple par ton lien CELCAT dans %s.\n", conf);
        return 1;
    } else if (c.url[0]) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        texte = telecharger(c.url);
        curl_global_cleanup();
    } else if (access(conf, R_OK) != 0) {
        fprintf(stderr, "Fichier %s introuvable. Crée-le à partir de l'exemple :\n"
                        "  cd %s && cp emargement.conf.exemple emargement.conf\n"
                        "puis complète-le (voir README.md).\n", conf, projet);
        return 1;
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
    f.conf = &c;
    /* L'année scolaire commence en septembre */
    int rentree = c.mois >= 9 ? c.annee : c.annee - 1;
    snprintf(f.annee, sizeof f.annee, "%d/%d", rentree, rentree + 1);
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
        l->signe_matin = j->matin.nb > 0;
        l->signe_aprem = j->aprem.nb > 0;

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

    /* Signature (optionnelle) : ajoutée à l'archive et placée par cadre_signature */
    ods_fichier image = {0};
    if (c.signature[0]) {
        const char *type_mime = NULL;
        double ratio = ratio_image(c.signature, &type_mime);
        if (ratio > 0) {
            f.sig.type_mime = type_mime;
            f.sig.chemin = !strcmp(type_mime, "image/png") ? "Pictures/signature.png"
                                                           : "Pictures/signature.svg";
            f.sig.hauteur = HAUTEUR_LIGNE - 2 * MARGE_SIGNATURE;
            f.sig.largeur = f.sig.hauteur * ratio;
            image = (ods_fichier){c.signature, f.sig.chemin, type_mime};
        } else {
            fprintf(stderr, "Signature %s ignorée : formats acceptés SVG et PNG.\n", c.signature);
        }
    }

    if (ods_remplir(c.modele, c.sortie, resoudre, &f, image.source ? &image : NULL) != 0) return 1;

    printf("\n%d jour(s), %ldh%02ld de cours, %d séance(s) ignorée(s) -> %s\n",
           n, total_mois / 60, total_mois % 60, ignores, c.sortie);

    if (!c.sans_pdf) {
        char pdf[PATH_MAX];
        if (convertir_pdf(c.sortie, pdf, sizeof pdf) == 0) printf("PDF -> %s\n", pdf);
    }
    return 0;
}
