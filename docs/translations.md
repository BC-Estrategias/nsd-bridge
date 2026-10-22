# Translations (3DS)

The **Translations** screen installs game translations that are published as [Luma3DS](https://github.com/LumaTeam/Luma3DS)
*LayeredFS* patches (the files Luma reads from `/luma/titles/<TITLE ID>/` instead of the game's own) from a folder you host
yourself. NSD Bridge ships with **no** translation files: it reads a WebDAV share (a Nextcloud public link). Builds from
2.0.0 on carry a default **read-only** link to BC Labs' share; **SELECT** in the screen types another one (`-` goes back to the
default), and a build made without the default has no catalog until you type one.

> Luma3DS must have **Enable game patching** turned on (hold SELECT while powering on to open its settings). NSD Bridge only
> reminds you; it never changes Luma's settings.

## Where the catalog is

In the console's `/3ds/nintendo-dev-agent/config/translations-url` there is one line: the public share link, for example
`https://cloud.example.com/s/AbCdEf123` (when the file does not exist the build's default link is used). The mods catalog has its
own, `config/mods-url`. The share
must contain a `traducoes/` folder laid out as below. (Only Nextcloud / ownCloud public links are understood for now: the
link's last part is the WebDAV user name, the password is empty.)

## Folder layout

```
traducoes/
  <language>/                               ptbr, en, es ... lower case, 2 to 8 characters
    <TITLE ID> - <game name>/               16 hex digits, " - ", the name shown in the list
      <team>/                               any text without "/" (jumpmanclub-brasil, e4t, desconhecida)
        v<version>/                         v1.2, v2, v1.10 (numbers compare as numbers)
          pacote.zip                        the pack as ONE file (recommended): paths relative to /luma/titles/<TITLE ID>/
          arquivos/                         or the same files loose: mirror of /luma/titles/<TITLE ID>/ (used when there is no pacote.zip)
          extras/                           anything else (manuals, covers, readmes): ignored
          pacote.json                       optional
```

* The **title ID is the identity**; the game name is only for display (split at the first `" - "`).
* A region is a different title ID. A translation that serves two regions has one folder per ID (or a second one with the
  same files); Bravely Default is an example.
* Newer versions of the same (ID, team) are offered as **updates**.

### `pacote.json` (optional)

```json
{
 "formato": 1,
 "jogo": "Pokemon X",
 "idioma": "pt-BR",
 "versao": "v1.4",
 "equipe": "E4T (Elite dos Quatro Traduções)",
 "regiao": "Livre",
 "fonte": "https://example.com/where-it-came-from"
}
```

Read fields: `equipe` (or `autor`, for a mod), `regiao` and `fonte` (shown on the confirmation screen), `descricao` (a short
description, under the game's name in the list) and `status` (a warning shown in yellow, for example *experimental*). Without the
file the folder names are used.

### `pacote.zip` (recommended)

One file instead of thousands of requests. Zip the *contents* of the pack (the `romfs/` and `exefs/` folders, `code.ips`...), stored
or deflate, no password, no zip64; a leading `arquivos/` folder inside the zip is accepted. Folder entries, `__MACOSX/`, `._*`,
`.DS_Store`, `Thumbs.db` and `desktop.ini` are ignored. The console downloads it to `/3ds/nintendo-dev-agent/downloads/` (continuing
a part left there when the share's ETag is the same), reads the index for the confirmation screen, extracts it and deletes it; if
the extraction is stopped the zip stays and the next install needs no download. A zip with an unsafe path, a repeated name,
encryption, another method, or more than 20 000 files / 1.5 GiB is refused. Creating a folder takes seconds on a 3DS, so a pack
with many folders is slow however it is delivered: the confirmation screen says how many it has to create.

## What the console does

1. **Catalog:** reads the folders (WebDAV `PROPFIND`, one level at a time) and keeps a saved copy in
   `/3ds/nintendo-dev-agent/cache/translations.txt` together with the folder's tag (its ETag). The saved list shows at once when
   the screen opens; one small request asks whether the tag changed and, if so, the list is read again. If the server does not
   answer, a red line says so and the saved list stays. **Y** reads again regardless.
2. **Marks:** `★` a pack NSD Bridge installed (the version is shown, with *update* if the share has a newer one);
   `☆` a folder already exists in `/luma/titles/<ID>/` but was put there by hand.
3. **Install:** lists `arquivos/`, shows what will be written (files, size, the game name, the credits, and a warning if the
   title is not installed on this console), and waits for A. The record `/luma/titles/<ID>/.nsd-pack.json` (details and the
   list of files, marked "incomplete") is written first and the files are downloaded straight to their names, six at once,
   each one tried again up to three times after a server error or a dropped connection; when everything is there the record
   is marked complete. An install that was interrupted shows `!` in the list and is finished by installing again. The record
   is what makes *Uninstall* and the update check possible.
4. **Uninstall:** with the record, removes exactly the files the pack wrote. The folders stay (empty), because creating them again is
   the slow part of an install; `.nsd-empty` marks them so they are not taken for a hand-made patch. For a `☆` pack (no
   record) it removes only the files that also appear in the share's list for that pack; never the whole folder.

## Mods

A second catalog with the same screens and rules, laid out as `mods/<TITLE ID> - <game name>/<mod>/v<version>/{arquivos,extras,pacote.json}`
(`pacote.json` may carry `autor` instead of `equipe`). A mod may replace files that exist already, so the install copies each
original to `/luma/titles/<ID>/.nsd-backup/<mod>/<path>` first (the confirmation says how many files will be replaced and how many
of them belong to another installed pack), rolls back if anything fails, and *Uninstall* puts the originals back. Its record is
`/luma/titles/<ID>/.nsd-mod-<mod>.json`. A mod's files are written to a temporary name and renamed, so a replaced file is either
complete or untouched.

## Safety rules

* Writes only under `/luma/titles/<ID>/`, where `<ID>` comes from a folder name that matched `[0-9A-F]{16}`.
* Every file path is checked: relative, no `..`, no `.`, no empty or very long segments, no `\ : * ? " < > |`, no control
  characters, no names FAT would silently change (trailing space or dot).
* `._*`, `.DS_Store`, `Thumbs.db` and `desktop.ini` are never copied.
* Limits per install: 20 000 files and 1.5 GiB; it needs that much plus 16 MiB free on the card.
* HTTPS with the embedded Mozilla CA bundle; nothing is installed without the on-screen confirmation.
* The remote command channel (page, `ndev`, MCP) cannot trigger this; it only runs from the console's own screen.
