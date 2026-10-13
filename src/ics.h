#ifndef ICS_H
#define ICS_H

#include <stddef.h>
#include <time.h>

/* Type de séance, dans l'ordre de priorité pour le choix du formateur */
typedef enum {
    SEANCE_TD = 0,
    SEANCE_TP = 1,
    SEANCE_CM = 2,
    SEANCE_PARTIEL = 3,
    SEANCE_AUTRE = 99   /* Réunion, etc. : ignorée */
} type_seance;

typedef struct {
    time_t debut;
    time_t fin;
    type_seance type;
    char type_brut[64];    /* ex. "TD", "Contrôle Partiel"            */
    char matiere[160];     /* ex. "KINX7AD1 - Parallélisme"            */
    char formateur[96];    /* premier enseignant de DESCRIPTION, ou "" */
} seance;

typedef struct {
    seance *v;
    size_t n, cap;
} liste_seances;

/* Analyse un flux iCalendar (texte complet en mémoire).
   Retourne 0 si OK. Les séances annulées (STATUS:CANCELLED) sont écartées. */
int ics_parser(const char *texte, liste_seances *out);

void liste_liberer(liste_seances *l);

const char *type_nom(type_seance t);

#endif
