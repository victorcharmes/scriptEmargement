# emargement

Remplit automatiquement la feuille d'émargement d'alternance à partir de ton emploi du temps
CELCAT : dates, horaires, nom des formateurs et durées. Tu obtiens un `.pdf` prêt à imprimer
(A4 paysage, une page) et un `.ods` modifiable dans LibreOffice Calc.

Il te reste juste à faire signer.

---

## Démarrage rapide

Une fois l'installation faite (voir plus bas), il suffit de trois étapes :

```bash
cp emargement.conf.exemple emargement.conf   # 1. crée ta configuration
nano emargement.conf                         # 2. complète-la (lien CELCAT, nom, formation…)
./emargement                                 # 3. génère la feuille
```

---

## 1. Installation

Le programme tourne sous **Linux** (Ubuntu, Debian, Fedora, Arch…) et sous **Windows avec WSL**.
Il ne fonctionne ni sous Windows sans WSL, ni sous macOS pour l'instant.

L'installation se fait en deux temps : installer les paquets selon ton système (étape A), puis
récupérer et compiler le programme (étape B, la même pour tout le monde).

### A. Installer les paquets

<details>
<summary><b>Windows (WSL)</b></summary>

Si tu n'as pas encore WSL : ouvre PowerShell **en administrateur**, lance `wsl --install`,
redémarre, puis ouvre « Ubuntu » depuis le menu Démarrer et choisis un nom d'utilisateur et un mot
de passe. Toutes les commandes qui suivent se tapent dans ce terminal Ubuntu.

```bash
sudo apt update
sudo apt install -y git build-essential pkg-config libcurl4-openssl-dev libzip-dev
sudo apt install -y libreoffice-calc-nogui fonts-crosextra-carlito
```

</details>

<details>
<summary><b>Linux : Ubuntu, Debian, Linux Mint, Pop!_OS…</b></summary>

Ouvre un terminal :

```bash
sudo apt update
sudo apt install -y git build-essential pkg-config libcurl4-openssl-dev libzip-dev
sudo apt install -y libreoffice-calc-nogui fonts-crosextra-carlito
```

Si `libreoffice-calc-nogui` est introuvable (distribution ancienne), installe `libreoffice-calc`
à la place. Si LibreOffice est déjà installé sur ton PC, il sera utilisé tel quel : seule la police
`fonts-crosextra-carlito` reste nécessaire.

</details>

<details>
<summary><b>Linux : Fedora</b></summary>

```bash
sudo dnf install -y git gcc make pkgconf-pkg-config libcurl-devel libzip-devel
sudo dnf install -y libreoffice-calc google-carlito-fonts
```

</details>

<details>
<summary><b>Linux : Arch, Manjaro, EndeavourOS</b></summary>

```bash
sudo pacman -S --needed git base-devel curl libzip
sudo pacman -S --needed libreoffice-fresh ttf-carlito
```

</details>

Pour une autre distribution, il te faut l'équivalent de : un compilateur C et `make`,
`pkg-config`, les fichiers de développement de **libcurl** et **libzip**, **LibreOffice Calc**
et la police **Carlito**.

**À quoi servent ces paquets ?**
- **git, compilateur, pkg-config** : récupérer et compiler le programme.
- **libcurl, libzip** : télécharger l'emploi du temps et écrire le fichier `.ods`.
- **LibreOffice Calc** : convertir la feuille en PDF, sans ouvrir de fenêtre.
- **Carlito** : remplace Calibri, la police du modèle. Sans elle, la mise en page du PDF est décalée.

### B. Récupérer et compiler (tous les systèmes)

```bash
git clone https://github.com/victorcharmes/scriptEmargement.git
cd scriptEmargement
make
make test   # vérifie que tout marche : crée test/emargement_2026-10.ods et .pdf
```

Si `make test` se termine par `PDF -> test/emargement_2026-10.pdf`, l'installation est terminée.
Tu peux ouvrir ce PDF pour voir à quoi ressemble la feuille.

### Mettre à jour

Pour récupérer une nouvelle version du programme :

```bash
cd scriptEmargement
git pull
make
```

Ta configuration et ta signature ne sont pas touchées.

---

## 2. Configuration

Crée ton fichier de configuration à partir de l'exemple :

```bash
cp emargement.conf.exemple emargement.conf
nano emargement.conf     # ou n'importe quel éditeur
```

Puis complète les champs suivants :

| Champ         | À mettre                                                    | Exemple                          |
| ------------- | ------------------------------------------------------------ | -------------------------------- |
| `url`       | le lien`.ics` de ton emploi du temps CELCAT (voir dessous) | `https://…/ical?…`           |
| `nom`       | ton nom, tel qu'il doit apparaître sur la feuille           | `Camille MARTIN`               |
| `formation` | l'intitulé de ta formation                                  | `M1 INFO PARC SDL`             |
| `organisme` | l'organisme de formation                                     | `Université de Toulouse`      |
| `existence` | son numéro de déclaration d'existence                      | `76311365631`                  |
| `siret`     | son numéro SIRET                                            | `93827139200012`               |
| `dossier`   | *(optionnel)* où écrire les fichiers                     | `/mnt/c/Users/camille/Desktop` |
| `signature` | *(optionnel)* ta signature, en SVG ou PNG                  | `signature.svg`                |

Les valeurs `organisme`, `existence` et `siret` de l'exemple sont celles de l'Université de
Toulouse : ne les change que si tu es dans un autre établissement.

`emargement.conf` contient ton lien CELCAT personnel. Il est exclu de git (`.gitignore`) :
ne le partage pas.

### Trouver son lien CELCAT

Va sur **[https://edt.univ-tlse3.fr/icalfeed/](https://edt.univ-tlse3.fr/icalfeed/)** et connecte-toi avec ton compte étudiant :
la page te donne le lien iCalendar de ton emploi du temps. Copie-le dans `url=`.

C'est le même lien que celui qu'on colle dans Google Agenda (« Autres agendas » → « À partir de
l'URL ») ou Outlook pour s'abonner à son emploi du temps. Pour vérifier qu'il est bon, ouvre-le
dans un navigateur : le fichier téléchargé doit commencer par `BEGIN:VCALENDAR`.

### Ajouter sa signature

Place ta signature dans le dossier du projet (format **SVG** de préférence, ou **PNG** sur fond
transparent), puis indique-la dans la conf :

```
signature=signature.svg
```

Elle est alors insérée automatiquement dans les cases **Matin – Signature stagiaire** et
**Après-midi – Signature stagiaire** de chaque demi-journée de cours, centrée et à la hauteur de
la case. Les demi-journées sans cours restent vides. Sans `signature=`, ces cases restent à signer
à la main.

`signature.svg` et `signature.png` sont exclus de git : ta signature ne part pas sur GitHub.

### Choisir le dossier de sortie

Par défaut, les fichiers sont créés dans le dossier du projet. Avec `dossier=`, tu peux les écrire
ailleurs. Il faut donner un **chemin complet** (`~` n'est pas compris) vers un dossier **qui existe
déjà**.

- **Linux** : par exemple ton dossier Documents. Tape `echo ~/Documents` pour obtenir son chemin
  complet :
  ```
  dossier=/home/camille/Documents/emargement
  ```
- **WSL** : le disque `C:` de Windows est accessible sous `/mnt/c`, donc tu peux écrire directement
  sur le Bureau Windows :
  ```
  dossier=/mnt/c/Users/TON-NOM-WINDOWS/Desktop
  ```

---

## 3. Utilisation

Lance toujours le programme **depuis le dossier du projet** : il y cherche `emargement.conf` et le
modèle.

```bash
./emargement                 # mois automatique (voir ci-dessous)
./emargement -m 2026-11      # mois précis
```

**Mois automatique** : lancé entre le 1er et le 20, il génère le mois en cours ; après le 20, le
mois suivant. Ça marche donc aussi bien en début qu'en fin de mois.

Le terminal affiche un récapitulatif du mois, puis les fichiers produits :
`emargement_AAAA-MM.ods` et `emargement_AAAA-MM.pdf`.

> L'emploi du temps peut changer après la génération : relance le programme avant d'imprimer
> si des cours ont été déplacés.

### Toutes les options

| Option         | Effet                                               |
| -------------- | --------------------------------------------------- |
| `-m AAAA-MM` | mois à générer                                   |
| `-d DOSSIER` | dossier de sortie (remplace`dossier=` de la conf) |
| `-o FICHIER` | nom complet du`.ods` produit                      |
| `-P`         | ne pas générer le PDF                             |
| `-n NOM`     | nom du stagiaire (remplace`nom=` de la conf)      |
| `-u URL`     | lien CELCAT (remplace`url=` de la conf)           |
| `-f FICHIER` | lire un fichier`.ics` local au lieu du lien       |
| `-c CONF`    | autre fichier de configuration                      |
| `-t MODELE`  | autre modèle`.ods`                               |

---

## Ce qui est rempli

| Colonne                                    | Contenu                                                                                                                                                |
| ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| En-tête                                   | organisme, n° d'existence, SIRET, formation, nom (depuis la conf)                                                                                     |
| Année / Période                          | calculées :`2026/2027` (rentrée en septembre), `Octobre 2026`                                                                                    |
| Date                                       | `jeu. 01/10/2026`, une ligne par jour avec au moins un cours                                                                                         |
| Matin – Horaires                          | séances commençant avant 12h30, un créneau par ligne dans la cellule                                                                                |
| Matin – Nom du formateur                  | enseignant du**TD**, sinon du **TP**, sinon du **CM**, sinon du partiel ; vide si la séance retenue n'a pas d'enseignant renseigné |
| Après-midi – Horaires / Formateur        | même principe                                                                                                                                         |
| Matin / Après-midi – Signature stagiaire | ta signature si`signature=` est renseigné, pour chaque demi-journée de cours                                                                       |
| Durée totale de la journée               | somme des créneaux, ex.`5h30`                                                                                                                       |

- Seules les séances **Cours (CM), TD, TP, Contrôle Partiel et Examen** sont prises. Le reste
  (Réunion…) est ignoré et listé dans le terminal.
- Les séances annulées sont ignorées.
- Les séances qui se chevauchent sont fusionnées : un partiel réparti sur 4 salles ne compte
  qu'une fois.
- Les heures CELCAT (UTC) sont converties en heure de Paris, heure d'été et d'hiver comprises.
- La feuille a 23 lignes. Au-delà, un avertissement s'affiche.

---

## En cas de problème

| Message / symptôme                                 | Solution                                                                |
| --------------------------------------------------- | ----------------------------------------------------------------------- |
| `Fichier emargement.conf introuvable`             | `cp emargement.conf.exemple emargement.conf`, puis complète-le       |
| `Remplace l'URL d'exemple par ton lien CELCAT`    | mets ton vrai lien dans`url=`                                         |
| `Téléchargement impossible`                     | lien faux ou expiré, ou pas de connexion : teste-le dans un navigateur |
| `LibreOffice introuvable : PDF non généré`     | `sudo apt install libreoffice-calc-nogui`                             |
| Titre qui se chevauche, en-têtes sur 3 lignes      | police manquante :`sudo apt install fonts-crosextra-carlito`          |
| `Signature … ignorée`                           | la signature doit être un SVG ou un PNG                                |
| `make` : `curl/curl.h` ou `zip.h` introuvable | relance la ligne`sudo apt install … libcurl4-openssl-dev libzip-dev` |

### Conversion PDF

Le PDF est produit par LibreOffice sans interface, à partir du `.ods`. Le programme utilise le
LibreOffice de Linux/WSL (environ 2 s). S'il est absent, il se rabat sur celui de Windows
(`C:\Program Files\LibreOffice`), plus lent (8 à 10 s). Si la conversion échoue, le `.ods` est
quand même généré.

---

## Personnaliser la feuille

Ouvre `modele_emargement.ods` dans LibreOffice Calc, modifie la mise en page (logo, couleurs,
largeurs…) **sans toucher aux balises `{{...}}`**, puis enregistre au format ODS.

| Balise                                                                                       | Remplacée par                                                           |
| -------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| `{{NOM}}` `{{FORMATION}}` `{{ORGANISME}}` `{{EXISTENCE}}` `{{SIRET}}`              | les champs de la conf                                                    |
| `{{ANNEE}}` `{{PERIODE}}`                                                                | l'année scolaire et le mois                                             |
| `{{D01}}` `{{MH01}}` `{{MF01}}` `{{AH01}}` `{{AF01}}` `{{DT01}}` … `{{…23}}` | date, horaires et formateur du matin/après-midi, durée de chaque ligne |
| `{{MS01}}` `{{AS01}}` … `{{…23}}`                                                    | signature du stagiaire, matin et après-midi                             |

Le logo est celui de l'Université de Toulouse. Pour un autre établissement, remplace l'image
dans le modèle.

Si tu changes la largeur des colonnes « Signature stagiaire » ou la hauteur des lignes, mets à jour
`LARGEUR_COL_MS`, `LARGEUR_COL_AS` et `HAUTEUR_LIGNE` dans `src/main.c` pour que la signature
reste centrée.

## Lancement automatique chaque mois (optionnel)

Avec cron, la feuille peut être générée toute seule chaque mois :

- **Linux** : cron fonctionne directement. S'il n'est pas installé : `sudo apt install cron`
  (Ubuntu/Debian) ou `sudo dnf install cronie` puis `sudo systemctl enable --now crond` (Fedora).
- **WSL** : cron ne tourne que si systemd est activé (`/etc/wsl.conf` → `[boot]` `systemd=true`),
  et seulement quand WSL est ouvert.

Puis :

```bash
crontab -e
# le 25 de chaque mois à 9h, génère le mois suivant
0 9 25 * * cd ~/scriptEmargement && ./emargement >> emargement.log 2>&1
```

## Structure du code

```
src/main.c   configuration, options, regroupement par jour / demi-journée, choix du formateur, PDF
src/ics.c    lecture du flux iCalendar (dépliage des lignes, échappements, dates UTC)
src/ods.c    copie du modèle .ods et remplacement des balises {{...}} dans content.xml (libzip)
```
