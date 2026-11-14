#include "md3/theme.hpp"
#include <cstdio>
#include <cstdlib>
using namespace tnx; using namespace tnx::md3;
#define R(n) std::printf("%s\"" #n "\": \"#%02X%02X%02X\"", first?"":", ", s.n.r, s.n.g, s.n.b); first=false;
int main(int argc,char**argv){
  uint32_t seed=(uint32_t)strtoul(argv[1],nullptr,16)|0xFF000000u;
  std::printf("{");
  for(int d=0;d<2;d++){ ColorScheme s=schemeFromSeed(seed,d==0); bool first=true;
    std::printf("%s\"%s\": {", d?", ":"", d==0?"dark":"light");
    R(primary)R(onPrimary)R(primaryContainer)R(onPrimaryContainer)
    R(secondary)R(onSecondary)R(secondaryContainer)R(onSecondaryContainer)
    R(tertiary)R(onTertiary)R(tertiaryContainer)R(onTertiaryContainer)
    R(error)R(onError)R(errorContainer)R(onErrorContainer)
    R(background)R(onBackground)R(surface)R(onSurface)R(surfaceVariant)R(onSurfaceVariant)
    R(surfaceDim)R(surfaceBright)R(surfaceContainerLowest)R(surfaceContainerLow)R(surfaceContainer)
    R(surfaceContainerHigh)R(surfaceContainerHighest)R(outline)R(outlineVariant)
    R(inverseSurface)R(inverseOnSurface)R(inversePrimary)R(shadow)R(scrim)R(surfaceTint)
    std::printf("}");
  }
  std::printf("}\n");
}
