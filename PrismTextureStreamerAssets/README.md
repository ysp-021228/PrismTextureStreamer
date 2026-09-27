# PrismTextureStreamer development mod

This directory is an unpacked ETS2 mod package. Copy the whole directory to:

`Documents/Euro Truck Simulator 2/mod/PrismTextureStreamerAssets`

Enable `PrismTextureStreamer GPS override` in the ETS2 Mod Manager.

The asset generator is `tools/generate_gps_assets.py`. It creates the checked-in GPS assets (`gps.dds`, `gps.tobj`) and Dashboard assets (`dashboard.dds`, `dashboard.tobj`). The Dashboard texture is independent: 2048x64, DXT5/BC3, 12 mip levels.
