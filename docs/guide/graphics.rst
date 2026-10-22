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
    VMA         = 0x1000    # refers to the base address used for generating the index symbols
    size        = 0x1000    # refers to the size of the entry, may be a Character ROM Window
    [includes]
        asset1 = "path/to/file1"
        asset2 = "path/to/file2"
        asset3 = "path/to/file3"

The resulting members will be generated:

.. code-block:: cpp

    blob::sprites::asset1;   // will be 0x00
    blob::sprites::asset2;
    blob::sprites::asset3;
    blob::sprites::__LMA;    // will be 0x1000

Note that with this, its incredibly easy to get the offset of a singular tile with:

.. code-block:: cpp

    #define CharacterROMAddrOf(nameSpace, member) \
        (nameSpace::__LMA + (nameSpace::member << 5))

This can be followed by:

.. code-block:: cpp

    #define PPUSpaceAddrOf(nameSpace, member) \
        (nameSpace::__VMA + (nameSpace::member << 5))

Then the values this returns can given to a mapper functions ``BankCharactersWith()`` method with the size being ``0``
to bank in what is needed.

Nothing prevents the active Character Window from loading information