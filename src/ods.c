#include "ods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zip.h>

/* ---- petit tampon extensible ---- */
typedef struct {
    char *p;
    size_t n, cap;
} tampon;

static int t_ajouter(tampon *t, const char *s, size_t n)
{
    if (t->n + n + 1 > t->cap) {
        size_t nc = t->cap ? t->cap : 4096;
        while (t->n + n + 1 > nc) nc *= 2;
        char *np = realloc(t->p, nc);
        if (!np) return -1;
        t->p = np;
        t->cap = nc;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = '\0';
    return 0;
}

static int t_ajouter_str(tampon *t, const char *s) { return t_ajouter(t, s, strlen(s)); }

/* Échappe le texte pour XML ; '\n' ferme le paragraphe et en ouvre un autre
   (la balise est toujours placée seule dans un <text:p> du modèle). */
static int ajouter_valeur(tampon *t, const char *v)
{
    for (; *v; v++) {
        int r;
        switch (*v) {
        case '&': r = t_ajouter_str(t, "&amp;"); break;
        case '<': r = t_ajouter_str(t, "&lt;"); break;
        case '>': r = t_ajouter_str(t, "&gt;"); break;
        case '"': r = t_ajouter_str(t, "&quot;"); break;
        case '\n': r = t_ajouter_str(t, "</text:p><text:p>"); break;
        default: r = t_ajouter(t, v, 1);
        }
        if (r) return -1;
    }
    return 0;
}

static char *remplacer(const char *xml, resolveur f, void *ctx)
{
    tampon t = {0};
    const char *p = xml;
    for (;;) {
        const char *a = strstr(p, "{{");
        if (!a) break;
        const char *b = strstr(a + 2, "}}");
        if (!b) break;
        if (t_ajouter(&t, p, (size_t)(a - p))) goto err;

        char cle[64];
        size_t n = (size_t)(b - a - 2);
        if (n >= sizeof cle) n = sizeof cle - 1;
        memcpy(cle, a + 2, n);
        cle[n] = '\0';

        const char *v = f(cle, ctx);
        p = b + 2;
        if (v && !strncmp(v, ODS_XML, strlen(ODS_XML))) {
            /* XML brut : on retire le <text:p> qui entoure la balise */
            static const char ouv[] = "<text:p>", ferm[] = "</text:p>";
            if (t.n >= strlen(ouv) && !strcmp(t.p + t.n - strlen(ouv), ouv) &&
                !strncmp(p, ferm, strlen(ferm))) {
                t.n -= strlen(ouv);
                t.p[t.n] = '\0';
                p += strlen(ferm);
            }
            if (t_ajouter_str(&t, v + strlen(ODS_XML))) goto err;
        } else if (v && ajouter_valeur(&t, v)) {
            goto err;
        }
    }
    if (t_ajouter_str(&t, p)) goto err;
    return t.p;
err:
    free(t.p);
    return NULL;
}

static int copier_fichier(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) { perror(src); return -1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { perror(dst); fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

/* Lit une entrée de l'archive dans un tampon terminé par '\0' (à libérer) */
static char *lire_entree(zip_t *z, const char *nom)
{
    zip_stat_t st;
    if (zip_stat(z, nom, 0, &st) != 0) {
        fprintf(stderr, "%s introuvable dans le modèle\n", nom);
        return NULL;
    }
    char *buf = malloc(st.size + 1);
    zip_file_t *zf = zip_fopen(z, nom, 0);
    if (!buf || !zf || zip_fread(zf, buf, st.size) != (zip_int64_t)st.size) {
        fprintf(stderr, "Lecture de %s impossible\n", nom);
        if (zf) zip_fclose(zf);
        free(buf);
        return NULL;
    }
    zip_fclose(zf);
    buf[st.size] = '\0';
    return buf;
}

/* Remplace (ou ajoute) une entrée ; `buf` est libéré par libzip dans tous les cas */
static int ecrire_entree(zip_t *z, const char *nom, char *buf, size_t n)
{
    zip_source_t *src = zip_source_buffer(z, buf, n, 1);
    if (!src) { free(buf); return -1; }
    zip_int64_t idx = zip_file_add(z, nom, src, ZIP_FL_OVERWRITE | ZIP_FL_ENC_UTF_8);
    if (idx < 0) {
        fprintf(stderr, "Écriture de %s impossible : %s\n", nom, zip_strerror(z));
        zip_source_free(src);
        return -1;
    }
    zip_set_file_compression(z, (zip_uint64_t)idx, ZIP_CM_DEFLATE, 0);
    return 0;
}

/* Ajoute un fichier du disque dans l'archive et le déclare dans le manifeste */
static int ajouter_fichier(zip_t *z, const ods_fichier *a)
{
    FILE *in = fopen(a->source, "rb");
    if (!in) { perror(a->source); return -1; }
    tampon t = {0};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (t_ajouter(&t, buf, n)) { fclose(in); free(t.p); return -1; }
    fclose(in);
    if (ecrire_entree(z, a->chemin, t.p, t.n)) return -1;

    char *manif = lire_entree(z, "META-INF/manifest.xml");
    if (!manif) return -1;
    char *fin = strstr(manif, "</manifest:manifest>");
    if (!fin) { free(manif); return -1; }
    tampon m = {0};
    char entree[512];
    snprintf(entree, sizeof entree,
             " <manifest:file-entry manifest:full-path=\"%s\" manifest:media-type=\"%s\"/>\n",
             a->chemin, a->type_mime);
    int r = t_ajouter(&m, manif, (size_t)(fin - manif)) || t_ajouter_str(&m, entree) ||
            t_ajouter_str(&m, fin);
    free(manif);
    if (r) { free(m.p); return -1; }
    return ecrire_entree(z, "META-INF/manifest.xml", m.p, m.n);
}

int ods_remplir(const char *modele, const char *sortie, resolveur f, void *ctx,
                const ods_fichier *ajout)
{
    /* On part d'une copie exacte du modèle : mimetype, styles, logo, mise en page
       restent intacts, seul content.xml est remplacé (plus l'éventuel ajout). */
    if (copier_fichier(modele, sortie)) return -1;

    int err = 0;
    zip_t *z = zip_open(sortie, 0, &err);
    if (!z) {
        fprintf(stderr, "Impossible d'ouvrir %s comme archive ODS (code %d)\n", sortie, err);
        return -1;
    }

    char *xml = lire_entree(z, "content.xml");
    if (!xml) { zip_discard(z); return -1; }
    char *nouveau = remplacer(xml, f, ctx);
    free(xml);
    if (!nouveau || ecrire_entree(z, "content.xml", nouveau, strlen(nouveau)) ||
        (ajout && ajouter_fichier(z, ajout))) {
        zip_discard(z);
        return -1;
    }

    if (zip_close(z) != 0) {
        fprintf(stderr, "Écriture de %s impossible : %s\n", sortie, zip_strerror(z));
        zip_discard(z);
        return -1;
    }
    return 0;
}
