# CPU rider appearance

Gameplay in the launcher overlay contains Random CPU Rider Skins, CPU Skin
Randomization (0–100%) and CPU Rider Skin Pool (Default, More Characters, Both).
Settings apply next race in all race modes. The percentage chooses an exact,
rounded count from all eligible CPU racers: ten CPU riders at 50% changes five.
Humans and police are excluded. Native identities, bike families, names,
physics and AI behavior remain unchanged. Online choices are local cosmetics.

Original rider textures are read from the user's supported ROM at startup.
Nothing is exported or shipped. Replacement skins use matching male/female
native geometry. More Characters must be enabled for its skins to enter the
pool; an empty selected pool leaves the original appearances intact.

The roster is ranked with a cosmetic seed independent of native gameplay RNG.
The seed and settings remain fixed through the race and highlights. Renderer
storage is bounded to 32 custom appearances and 40 original appearances.
Tests cover exact percentages, both pools, human/police exclusion, all forty
ROM texture records, native rendering and selector regressions.
