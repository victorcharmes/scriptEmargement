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
        if (v && ajouter_valeur(&t, v)) goto err;
        p = b + 2;
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

int ods_remplir(const char *modele, const char *sortie, resolveur f, void *ctx)
{
    /* On part d'une copie exacte du modèle : mimetype, styles, logo, mise en page
       restent intacts, seul content.xml est remplacé. */
    if (copier_fichier(modele, sortie)) return -1;

    int err = 0;
    zip_t *z = zip_open(sortie, 0, &err);
    if (!z) {
        fprintf(stderr, "Impossible d'ouvrir %s comme archive ODS (code %d)\n", sortie, err);
        return -1;
    }

    zip_stat_t st;
    if (zip_stat(z, "content.xml", 0, &st) != 0) {
        fprintf(stderr, "content.xml introuvable dans le modèle\n");
        zip_discard(z);
        return -1;
    }
    char *xml = malloc(st.size + 1);
    zip_file_t *zf = zip_fopen(z, "content.xml", 0);
    if (!xml || !zf || zip_fread(zf, xml, st.size) != (zip_int64_t)st.size) {
        fprintf(stderr, "Lecture de content.xml impossible\n");
        if (zf) zip_fclose(zf);
        free(xml);
        zip_discard(z);
        return -1;
    }
    zip_fclose(zf);
    xml[st.size] = '\0';

    char *nouveau = remplacer(xml, f, ctx);
    free(xml);
    if (!nouveau) { zip_discard(z); return -1; }

    /* libzip libère `nouveau` lui-même (freep = 1) */
    zip_source_t *src = zip_source_buffer(z, nouveau, strlen(nouveau), 1);
    zip_int64_t idx = zip_name_locate(z, "content.xml", 0);
    if (!src || idx < 0 || zip_file_replace(z, (zip_uint64_t)idx, src, 0) != 0) {
        fprintf(stderr, "Remplacement de content.xml impossible : %s\n", zip_strerror(z));
        if (src) zip_source_free(src);
        zip_discard(z);
        return -1;
    }
    zip_set_file_compression(z, (zip_uint64_t)idx, ZIP_CM_DEFLATE, 0);

    if (zip_close(z) != 0) {
        fprintf(stderr, "Écriture de %s impossible : %s\n", sortie, zip_strerror(z));
        zip_discard(z);
        return -1;
    }
    return 0;
}
