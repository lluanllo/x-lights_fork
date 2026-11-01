# xLights translations

The interface uses wxWidgets gettext catalogs. Add `_()` around new user-facing
strings in C++ so the extractor can include them in the catalog.

Refresh the Spanish catalog template and preserve existing translations with:

```powershell
python ci_scripts/localization/build_catalog.py --extract resources/locale/xLights.pot
python ci_scripts/localization/build_catalog.py --update resources/locale/es.po
```

Translate any new empty `msgstr` entries in `es.po`. Visual Studio and CMake
compile the catalog to `locale/es/LC_MESSAGES/xLights.mo` beside the executable.
The application starts in Spanish by default; choose English in Preferences >
Other to change the interface language after restarting xLights.

The initial Spanish catalog is a machine-translated draft. Please review wording,
keyboard shortcuts, technical terms, and format placeholders before release.
