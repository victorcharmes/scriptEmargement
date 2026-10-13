#ifndef ODS_H
#define ODS_H

/* Fonction qui donne la valeur d'une balise {{CLE}} du modèle.
   Retourne NULL si la clé est inconnue (la balise est alors vidée).
   Les retours à la ligne '\n' deviennent des paragraphes dans la cellule.
   Une valeur qui commence par ODS_XML est du XML brut (ex. une image) : elle
   remplace tout le paragraphe <text:p>{{CLE}}</text:p>, sans échappement. */
typedef const char *(*resolveur)(const char *cle, void *ctx);

#define ODS_XML "\x01"

/* Fichier à ajouter dans l'archive (ex. une image référencée par le XML brut) */
typedef struct {
    const char *source;     /* chemin sur le disque              */
    const char *chemin;     /* chemin dans l'archive, ex. "Pictures/signature.svg" */
    const char *type_mime;  /* ex. "image/svg+xml"                */
} ods_fichier;

/* Copie le modèle .ods vers `sortie` en remplaçant les balises de content.xml.
   `ajout` (peut être NULL) est ajouté à l'archive et déclaré dans le manifeste. */
int ods_remplir(const char *modele, const char *sortie, resolveur f, void *ctx,
                const ods_fichier *ajout);

#endif
