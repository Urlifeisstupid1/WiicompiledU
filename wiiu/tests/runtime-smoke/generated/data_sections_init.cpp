// Stand-in for the translator's generated/data_sections_init.cpp, used only by the
// Wii U runtime smoke test: there are no game data sections to copy into guest RAM.
extern "C" void InitializeDataSections() {}
extern "C" bool IsDataSectionsInitialized() { return true; }
