#ifndef ODS_H
#define ODS_H

/* Fonction qui donne la valeur d'une balise {{CLE}} du modèle.
   Retourne NULL si la clé est inconnue (la balise est alors vidée).
   Les retours à la ligne '\n' deviennent des paragraphes dans la cellule. */
typedef const char *(*resolveur)(const char *cle, void *ctx);

/* Copie le modèle .ods vers `sortie` en remplaçant les balises de content.xml. */
int ods_remplir(const char *modele, const char *sortie, resolveur f, void *ctx);

#endif
