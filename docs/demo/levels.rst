Levels
======

The level data is tile based using ``16x16`` 'metatiles' stored in an 'AoS' layout using
c++ compile time technology from an easy to edit base array of ``1280`` bytes of data.

Each metatile is comprised of its four corner graphical tiles ``8x8`` and attribute data.
The graphical tile data is symbolically referenced from the CHR blob's generated header.

``demo/chr/config.toml`` declares where each asset is packed into the CHR ROM, and
``blobman`` (``tools/blobman``) packs it and emits ``demo/chr/chr.h`` beside the blob:

.. code-block:: toml

 [staticLevelTiles]
 LMA         = 0x1400
 VMA         = 0x0400
 size        = 0x0400
 includes    = [
     "./assets/tiles/static/terrain.chr"
 ]

.. code-block:: cp

 namespace blob {
     namespace staticLevelTiles {
         constexpr unsigned char terrain = 0x51;
         constexpr unsigned long __lma   = 0x1400; // used for BankOf()
         constexpr unsigned long __vma   = 0x0400; // where the ppu sees the entry
         constexpr unsigned long __size  = 0x0400; // bytes the entry occupies
     }
 }

Using this layout, the metatile data can be easily read at a glance as:

.. code-block:: cp

     MT_SPLIT(blob::dynamicLevelTiles::coin), (MetatileCollision::Collect | 0b01),   // $02 (coin)

The metatile and graphics data are compiled successfully into the ROM regardless of
present level data, this can be used to generate a modern image file (png) with the use of
the ``metatiles_to_tileset.py`` python script found in ``demo/tiled``.

This modern image file is used to construct the tile sheet for 'tiled' to use, which is the
tile based level editor of my choice (its free and easy to use for projects with this scope).

In tiled I ensure I have three layers:
 - static plane
  - Uncompressed, is never modified (lives in ROM). These may have unique behaviors such as
    changing their collision state or animating but are never removed and exist on the back layer.
 - dynamic plane
  - RLE Compressed, has lengths in ROM and data in RAM, is used when the tile should be changed
    typically to be removed (eg a coin on collision to collect it).
 - object construction plane
  - contains pixel perfect depictions of triggers that cause one or more objects to spawn that use
    Object Attribute Memory (OAM) such as enemies, player spawn or NPCs.


We export to a tiled ``json`` format saved to ``demo/tiled/exports`` and run the ``rle_compress.py``
script in ``demo/tiled`` that creates binary blobs in ``demo/tiled/include``.

A binary blob includes:

============ ===============
blob name    description
============ ===============
*{name}_dl*  The Dynamic plane lengths
*{name}_dt*  The Dynamic plane tiles
*{name}_sl*  The Static plane lengths
*{name}_st*  The static plane tiles
*{name}_oc*  The Object Construction plane content
============ ===============

.. note::

 Dynamic plane tiles originate in ROM but are, on area load, copied into a dedicated
 bss buffer. Each export has one write to this buffer and therefore in order to prevent
 'infinite coin-ing' a transition from one visible area to another may not necessarily
 mandate inability to return, but between actual level data loads a mechanism should
 forbid re-access of loading the previous area before 'end of level' and timer expiry.

.. note::

 The Object Construction plane (OCP) consists of the data needed to *generate* the actors that
 live in the game world with the player but must also be copied into BSS to ensure that
 enemies have no capacity to infinitely reappear as that introduced actor scheduling overhead
 and may be perceived as error.