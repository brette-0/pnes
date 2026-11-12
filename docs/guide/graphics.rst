Graphics
========

For many, they will use integer literals for their tile indices and draw their graphics in one file that is included
into their project. In prior iterations ``pnes`` offered macros for graphical includes that would generate aliases for
the tile indices based on order of inclusion, but that was removed due to issues with Character ROM Bankswitching.

The suggested approach is to use the official internal tool for this issue, which is known as ``blobman``.
``blobman`` expects a very simple ``toml`` file that acts like a linker script for character ROM information containing
file paths for NES graphic format binary blobs. When successful, ``blobman`` exports a singular large Character ROM file
alongside a C++ header file which converts the intentions described in the config ``toml`` to tile index symbols.

An example of a config toml file

.. code-block:: toml

    # must always have a manifest table containing a size member as positive non zero integer
    # must always appear first, is considered a reserved object alias for this feature
    [manifest]
    size        = 0x2000


    # all root objects must be tables, but no requirements to their name
    [sprites]
    LMA         = 0x0000    # refers to the position in Character ROM, gaps are permitted
    size        = 0x1000    # refers to the size of the entry, may be a Character ROM Window
    [includes]
        asset1 = "path/to/file1"
        asset2 = "path/to/file2"
        asset3 = "path/to/file3"

The resulting members will be generated:

.. code-block:: cpp

    blob::sprites::lma::asset1;   // tile index in Character ROM; will be 0x00
    blob::sprites::lma::asset2;
    blob::sprites::lma::asset3;
    blob::sprites::__lma;         // byte offset of the entry; will be 0x0000
    blob::sprites::__size;

Every tile symbol is an index into Character ROM, never into a pattern table. Where the PPU sees a tile is not a
property of how it was packed, so the manifest does not say: whatever banks the entry in decides that, and a
``ppu::ChrWindow`` is what turns a Character ROM tile index into the index the hardware reads.

.. code-block:: cpp

    // the set is packed at __lma and banked in at PPU $0400, i.e. tile 0x40
    constexpr ppu::ChrWindow window{blob::sprites::__lma / 16, 0x40};

    const u8 tile = window + blob::sprites::lma::asset1;

``__lma`` in bytes is what a mapper banks by -- ``mmc3::BankOf<blob::sprites::__lma>()`` -- while the tile indices
under ``lma::`` are in tiles, since a CHR tile is 16 bytes and no sub-tile value means anything.

Nothing prevents the active Character Window from loading information