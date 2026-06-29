Important note: Always use a limiter when testing.

# Atlas 1.4.0 updates

- Added the option to scan the Downloads folder for Atlas presets, making it easier to pick up presets people send without manually moving files around first.
- Added a clearer preset browser order: Library, Bank, Category, then preset list.
- Added Library filtering for Factory, User, and Other preset locations.
- Added an Atlas save patch form inside the plugin instead of relying on the operating system save dialog.
- The save patch form now includes name, author, bank, category, and comma-separated tags.
- Preset tags are saved in `.atp` files and can be used as browser categories.
- User presets saved from the new form go under `~/Documents/Alessio Plugins/Atlas/User/Presets`.
- Fixed preset autoload focus so VoiceOver should stay on the preset list when moving through presets with the arrow keys.
- Optimized preset tag reading so Atlas scans only preset metadata instead of parsing full preset files on startup.
- Added a persistent preset index at `~/Documents/Alessio Plugins/Atlas/AtlasPresetIndex.db` so the browser can load cached Factory/User libraries, banks, categories, tags, and search data instead of rereading every preset each time.
- Improved preset browser performance when switching Library, Bank, and Category by caching preset metadata, using faster lookup tables, and coalescing rapid filter changes into one browser refresh.
- Fixed preset browsing so loading or autoloading a preset no longer changes the selected Library, Bank, or Category filter.
- Improved FM from oscillator and ring mod from oscillator labels so they say the actual source oscillator.
- Added oscillator 4 as an FM and ring mod source where it was missing.
- Added the granular oscillator as an FM and ring mod source.
- Updated oscillator modulation routing and cycle checks so the new FM/ring mod sources work correctly.
- Updated the README to describe the newer preset workflow, Downloads scanning, tags, and content paths.
