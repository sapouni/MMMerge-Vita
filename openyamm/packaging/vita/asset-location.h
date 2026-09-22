#ifndef OY_ASSET_LOCATION_H
#define OY_ASSET_LOCATION_H
/* One coherent content root. A partial data-folder installation reports missing files,
 * rather than silently mixing editions from two roots. No file moves or writes. */
static const char *oy_asset_root(int nativePaths, int (*present)(const char *))
{
    const char *probe = nativePaths ? "ux0:data/openyamm/assets/engine.zip" : "/data/assets/engine.zip";
    if (present(probe))
        return nativePaths ? "ux0:data/openyamm/assets" : "/data/assets";
    return nativePaths ? "app0:assets" : "/app/assets";
}
#endif
