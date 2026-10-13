## Utilisation

```bash
cp emargement.conf.exemple emargement.conf   # puis colle ton lien CELCAT dedans
./emargement                                 # mois automatique
./emargement -m 2026-11                      # mois précis
./emargement -f cours.ics -m 2026-11         # depuis un fichier .ics local
./emargement -P                              # sans PDF (seulement le .ods)
./emargement -d /mnt/c/Users/NOM/Downloads   # écrit les fichiers dans ce dossier
```

# emargement

Remplit automatiquement la feuille d'émargement d'alternance (format `.ods`, LibreOffice Calc)
à partir de l'emploi du temps CELCAT (flux iCalendar `.ics`).

## Ce qui est rempli

| Colonne                             | Contenu                                                                                   |
| ----------------------------------- | ----------------------------------------------------------------------------------------- |
| Date                                | `jeu. 01/10/2026` — une ligne par jour avec au moins un cours                          |
| Matin – Horaires                   | créneaux des séances commençant avant 12h30 (un par ligne dans la cellule)             |
| Matin – Nom du formateur           | enseignant du **TD**, sinon du **TP**, sinon du **CM**, sinon du partiel — vide si la séance retenue n'a pas d'enseignant renseigné |
| Après-midi – Horaires / Formateur | même principe                                                                            |
| Durée totale de la journée        | somme des créneaux, ex.`5h30`                                                          |
| Nom du stagiaire / Période         | depuis la conf, ex.`Octobre 2026`                                                       |

- Seules les séances **Cours (CM), TD, TP, Contrôle Partiel, Examen** sont prises ; le reste
  (Réunion…) est ignoré et listé dans le terminal.
- Les séances annulées (`STATUS:CANCELLED`) sont ignorées.
- Les séances qui se chevauchent sont fusionnées : un partiel réparti sur 4 salles compte une seule fois.
- Les heures CELCAT (UTC) sont converties en heure de Paris (heure d'été/hiver gérée).

## Installation (WSL Ubuntu)

```bash
sudo apt install build-essential pkg-config libcurl4-openssl-dev libzip-dev
sudo apt install libreoffice-calc-nogui fonts-crosextra-carlito   # conversion PDF (voir plus bas)
make
make test        # génère test/emargement_2026-10.ods et .pdf à partir des données d'octobre
```

**Mois automatique** : lancé entre le 1er et le 20, il génère le mois en cours ; après le 20, le mois
suivant. Il convient donc aussi bien en fin de mois qu'en début de mois.

Sortie : `emargement_AAAA-MM.ods` et `emargement_AAAA-MM.pdf`, prêts à imprimer (A4 paysage, une page).

Les fichiers sont écrits dans le dossier courant, ou dans `dossier=` de `emargement.conf` (option `-d`).
Par exemple, `dossier=/mnt/c/Users/NOM/Downloads` les dépose directement dans les Téléchargements de Windows.

### Conversion PDF

Le PDF est produit par LibreOffice en mode headless (sans fenêtre), à partir du `.ods` généré.
Le programme cherche `soffice` dans cet ordre :

1. **LibreOffice de WSL** (`libreoffice-calc-nogui`) : recommandé, ~2 s ;
2. **LibreOffice de Windows** (`C:\Program Files\LibreOffice`) : fonctionne sans rien installer
   dans WSL, mais plus lent (~8 à 10 s) car il démarre côté Windows.

Si aucun des deux n'est trouvé ou si la conversion échoue, le `.ods` est quand même généré.
Avec `-P`, on ne génère que le `.ods`.

Le modèle utilise la police **Calibri**, absente de WSL. Sans elle, LibreOffice la remplace par une
police plus large : le titre se chevauche et les en-têtes passent sur trois lignes. Le paquet
`fonts-crosextra-carlito` fournit **Carlito**, qui a exactement les mêmes dimensions que Calibri.

> L'emploi du temps peut changer après la génération : relance le programme avant d'imprimer
> si des cours ont été déplacés.

## Lancement automatique chaque mois (optionnel)

Sous WSL, cron ne tourne que si systemd est activé (`/etc/wsl.conf` → `[boot] systemd=true`). Ensuite :

```bash
crontab -e
# le 25 de chaque mois à 9h, génère le mois suivant
0 9 25 * * cd ~/emargement && ./emargement >> emargement.log 2>&1
```

## Structure

```
src/main.c   options, regroupement par jour / demi-journée, choix du formateur, durées
src/ics.c    lecture du flux iCalendar (dépliage des lignes, échappements, dates UTC)
src/ods.c    copie du modèle .ods et remplacement des balises {{...}} dans content.xml (libzip)
modele_emargement.ods   feuille vierge avec balises {{D01}}, {{MH01}}, {{MF01}}, {{AH01}}, {{AF01}}, {{DT01}}…
```

Pour modifier la mise en page, ouvre `modele_emargement.ods` dans LibreOffice, change ce que tu veux
sans toucher aux balises `{{...}}`, puis enregistre au format ODS.
