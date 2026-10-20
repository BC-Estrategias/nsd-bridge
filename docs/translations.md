# Translations (3DS)

The **Translations** screen installs game translations that are published as [Luma3DS](https://github.com/LumaTeam/Luma3DS)
*LayeredFS* patches (the files Luma reads from `/luma/titles/<TITLE ID>/` instead of the game's own) from a folder you host
yourself. NSD Bridge ships with **no** catalog and **no** translation files: you point it at your own WebDAV share
(a Nextcloud public link) and it reads what is there.

> Luma3DS must have **Enable game patching** turned on (hold SELECT while powering on to open its settings). NSD Bridge only
> reminds you; it never changes Luma's settings.

## Where the catalog is

In the console's `/3ds/nintendo-dev-agent/config/translations-url` there is one line: the public share link, for example
`https://cloud.example.com/s/AbCdEf123`. The *Translations* screen asks for it with the keyboard the first time. The share
must contain a `traducoes/` folder laid out as below. (Only Nextcloud / ownCloud public links are understood for now: the
link's last part is the WebDAV user name, the password is empty.)

## Folder layout

```
traducoes/
  <language>/                               ptbr, en, es ... lower case, 2 to 8 characters
    <TITLE ID> - <game name>/               16 hex digits, " - ", the name shown in the list
      <team>/                               any text without "/" (jumpmanclub-brasil, e4t, desconhecida)
        v<version>/                         v1.2, v2, v1.10 (numbers compare as numbers)
          arquivos/                         mirror of /luma/titles/<TITLE ID>/ : romfs/..., exefs/..., code.ips
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

Only the credits are read (`equipe`, `regiao`, `fonte`, shown on the confirmation screen). Without the file the folder names
are used.

## What the console does

1. **Catalog:** reads the folders (WebDAV `PROPFIND`, one level at a time) and keeps a saved copy in
   `/3ds/nintendo-dev-agent/cache/translations.txt`; *Refresh* reads again.
2. **Marks:** `★` a pack NSD Bridge installed (the version is shown, with *update* if the share has a newer one);
   `☆` a folder already exists in `/luma/titles/<ID>/` but was put there by hand.
3. **Install:** lists `arquivos/`, shows what will be written (files, size, the game name, the credits, and a warning if the
   title is not installed on this console), and waits for A. Each file is downloaded to a temporary name and renamed, so a
   file is either complete or untouched. When everything is there it writes `/luma/titles/<ID>/.nsd-pack.json` with the
   pack's details and the list of files, which is what makes *Uninstall* and the update check possible.
4. **Uninstall:** with the record, removes exactly the files the pack wrote (and folders left empty). For a `☆` pack (no
   record) it removes only the files that also appear in the share's list for that pack; never the whole folder.

## Safety rules

* Writes only under `/luma/titles/<ID>/`, where `<ID>` comes from a folder name that matched `[0-9A-F]{16}`.
* Every file path is checked: relative, no `..`, no `.`, no empty or very long segments, no `\ : * ? " < > |`, no control
  characters, no names FAT would silently change (trailing space or dot).
* `._*`, `.DS_Store`, `Thumbs.db` and `desktop.ini` are never copied.
* Limits per install: 20 000 files and 1.5 GiB; it needs that much plus 16 MiB free on the card.
* HTTPS with the embedded Mozilla CA bundle; nothing is installed without the on-screen confirmation.
* The remote command channel (page, `ndev`, MCP) cannot trigger this; it only runs from the console's own screen.
