"""Reading files out of an Xbox disc image (XDVDFS), with no tool but Python.

The layout is XboxDev/extract-xiso's: the volume descriptor at sector 32 of the
game partition ("MICROSOFT*XBOX*MEDIA", the root directory's sector and size, the
same 20 bytes again at its end), and each directory a binary tree of entries
(left and right subtree offsets in 4-byte units, the file's sector and size,
attributes, the name). The game partition is at 0 in an XISO and further in on
a full disc dump. As Halo3DS's Xiso.cs does, every bound is checked and no name
in the image becomes a path outside the output folder.
"""

import os
import struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
PARTITIONS = (0, 0xFD90000, 0x2080000, 0x18300000)
DIRECTORY = 0x10


class XisoError(ValueError):
    pass


class Xiso:
    def __init__(self, path):
        self.path = path
        self.files = {}         # lower-case path -> (offset, size, path as in the image)
        self.length = os.path.getsize(path)
        with open(path, 'rb') as f:
            for start in PARTITIONS:
                if start + 0x10000 + SECTOR > self.length:
                    continue
                f.seek(start + 0x10000)
                header = f.read(SECTOR)
                if header[:20] != MAGIC:
                    continue
                if header[2028:2048] != MAGIC:
                    raise XisoError('the volume descriptor is damaged')
                self.partition = start
                root_sector, root_size = struct.unpack_from('<II', header, 20)
                self._directory(f, root_sector, root_size, '', 0, set())
                return
        raise XisoError(f'{path} is not an Xbox disc image (no XDVDFS volume)')

    def _directory(self, f, sector, size, prefix, depth, seen):
        if depth > 24 or size > 16 << 20:
            raise XisoError('a directory is nested too deep or too large')
        if size == 0:
            return
        offset = self.partition + sector * SECTOR
        if offset + size > self.length:
            raise XisoError('a directory is past the end of the image')
        if (sector, size) in seen:
            raise XisoError('a directory is listed twice')
        seen.add((sector, size))
        f.seek(offset)
        table = f.read(size)
        pending, visited = [0], set()
        while pending:
            at = pending.pop()
            if at in visited or at + 14 > size:
                raise XisoError('the directory\'s links are damaged')
            visited.add(at)
            left, right, file_sector, file_size, attributes, length = struct.unpack_from('<HHIIBB', table, at)
            if left == 0xFFFF:
                continue        # an empty directory's single entry
            name = table[at + 14:at + 14 + length].decode('latin-1')
            if not name or name in ('.', '..') or any(c in name for c in '/\\:\0'):
                raise XisoError(f'unsafe name in the image: {name!r}')
            if left:
                pending.append(left * 4)
            if right:
                pending.append(right * 4)
            full = prefix + name
            if attributes & DIRECTORY:
                self._directory(f, file_sector, file_size, full + '/', depth + 1, seen)
            else:
                start = self.partition + file_sector * SECTOR
                if start + file_size > self.length:
                    raise XisoError(f'{full} is past the end of the image: a short dump')
                if full.lower() in self.files:
                    raise XisoError(f'{full} is in the image twice')
                self.files[full.lower()] = (start, file_size, full)

    def maps(self):
        """name -> path in the image, for the .map files beside ui.map."""
        folders = {p.rsplit('/', 1)[0] if '/' in p else '' for p in self.files if p.endswith('ui.map')}
        if len(folders) != 1:
            raise XisoError('the image does not have one maps folder with ui.map in it')
        folder = folders.pop()
        prefix = folder + '/' if folder else ''
        return {p[len(prefix):-4]: p for p in self.files
                if p.startswith(prefix) and p.endswith('.map') and '/' not in p[len(prefix):]}

    def read(self, path):
        start, size, _ = self.files[path.lower()]
        with open(self.path, 'rb') as f:
            f.seek(start)
            return f.read(size)


def write_xiso(path, files):
    """A minimal XISO of {path in the image: bytes} (for the tests)."""
    tree = {}
    for name, data in files.items():
        node = tree
        parts = name.split('/')
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = data
    image = bytearray(0x10000 + SECTOR)

    def place(data):
        image.extend(bytes((-len(image)) % SECTOR))
        sector = len(image) // SECTOR
        image.extend(data)
        image.extend(bytes((-len(image)) % SECTOR))
        return sector

    def directory(node):
        names = sorted(node)
        entries = {}
        for name in names:
            child = node[name]
            if isinstance(child, dict):
                sector, size = directory(child)
                entries[name] = (sector, size, DIRECTORY)
            else:
                entries[name] = (place(child), len(child), 0x20)
        # A degenerate tree: each entry's right subtree is the next one.
        table, offsets = bytearray(), []
        for name in names:
            offsets.append(len(table))
            table += bytes(14 + len(name))
            table += bytes((-len(table)) % 4)
        for i, name in enumerate(names):
            sector, size, attributes = entries[name]
            right = offsets[i + 1] // 4 if i + 1 < len(names) else 0
            struct.pack_into('<HHIIBB', table, offsets[i], 0, right, sector, size, attributes, len(name))
            table[offsets[i] + 14:offsets[i] + 14 + len(name)] = name.encode('latin-1')
        if not names:
            table = bytearray(b'\xff' * 14)
        return place(bytes(table)), len(table)

    root_sector, root_size = directory(tree)
    header = bytearray(SECTOR)
    header[:20] = MAGIC
    struct.pack_into('<II', header, 20, root_sector, root_size)
    header[2028:2048] = MAGIC
    image[0x10000:0x10000 + SECTOR] = header
    with open(path, 'wb') as f:
        f.write(image)
