"""Which game this is: the build the converter knows, checked by hash.

The hashes are the retail Xbox maps of Halo: Combat Evolved, build 01.10.12.2276
(the English North American release), as Halo3DS's setup tool checks them
(port/n3ds/setup/Resources/catalog.json, CC0, at Generalkidd/Halo3DS 7150977c):
each map as it is on the disc (zlib-compressed after its header) and as it is
once expanded. A map that matches neither is refused, so the swap is only ever
run on data its rules were made from.
"""

import hashlib
import zlib

BUILD = '01.10.12.2276'
HEADER_SIZE = 0x800

# name: (bytes on the disc, sha256 on the disc, bytes expanded, sha256 expanded)
MAPS = {
    'ui': (14145536, '35e3e560478d85178749be310ad13d6d6ecde618d32675261a3554592333a833',
        33582080, '8556b647b82742d07282fe4e6db0cf847b542c046484e91e71ec31fea5f5fd7e'),
    'a10': (173903872, 'f7461e5b6eaf0314c4f03a1c6e43752b06ae3e4782b70b240a8c15e5f634b3ce',
        274734080, '1ce33954aa12a2e0c21a07e20d0dc652b1b403e4ef1b98c067439a15545d9971'),
    'a30': (152610816, '4875fa105e6c517589a796cfdcc7fcaa07e98df13d14e601bfc9eb1eab7e30b5',
        213065216, '287959f34a7379a1dbec5fec15b4d94d17fe97ccd861f5c0c8fc80148b7ed293'),
    'a50': (176224256, 'f8ec33c0130191cf944ef7b4a3f210bb680cfd1341cdf3a6ef3447391049abe7',
        250138624, 'd7a3e80a86f2b0f23d855ce353aed3c0eb252145697797d87f25623dc470f38d'),
    'b30': (162877440, 'a6e2ae628f96b666922d6429541068c3e117e8aa5031d00d187b3c853630be0f',
        224887296, '74020202d3930ee52c1ee96a7d8e3ee4020ce31a434b3f870d71df72b9c3e4dd'),
    'b40': (186873856, '0a7b26d6d828e7b773d36d9d5c798b9bf4b143332301a0c0e7a3d685782eab48',
        279763456, '8c5e4aecbd0bc7e063feb7d0c6a3f6d8cff65bf1abec75286070ef9c85fd927b'),
    'c10': (166926336, 'd65683f92e2f78cf8ba4d31c26b212869652b5d5ec037add480865a49e18434e',
        252559360, '423d50a4d7fb7b3c280d7e0806d67e136b2b6a51468ed0be6ee9415e5f129e7d'),
    'c20': (76980224, '6560cd88af4c5b4e2f02f14017085a7b47e9a9622198c30f36b03d3d3943d6b8',
        142009856, '1c1c9a8ca882636a3e78cb1d14e25a09c4a11388f6381c5f74212d468b70e654'),
    'c40': (166295552, 'b5fb3db5b111280e8494c5ec7612604d7facd1e0a511987afa38a228a115a896',
        255975936, 'd7465f07a675a86785a329b9ab3d8a5a20b25b7ca7d05bafeb7ca87b02b20be9'),
    'd20': (131012608, '6b38b4776f4be1e9e12063db202e5809245aae0ded3d9cef56602d7fd69c96be',
        205501952, '30d8fc02bcd209a7ce03174d8cf6cf771dd3621837d9a2d02111718dcc5e792c'),
    'd40': (170405888, '7c6dccc9f928f4a44db992c4d6f5fa6191e9cfd331bb0d0b631540f0fc9bc796',
        274594816, '438452ef1896dd5bc2d3210b279f76e71b775f7eb2b7a8f8e29f4e5471d59a76'),
    'beavercreek': (22122496, 'c65ac17d830552cdebfc173c1e8aa17f1f009149138c2c8e72afda1f79f88a60',
        43565056, 'eaf90b2abc4fd295469eb2d640063618b0d14396f28e7d7f1758e834c061c50a'),
    'sidewinder': (24823808, '210f7a01bac7b4c242483b1a640e0bc32fb0a2b4f97f56e69668720563b34912',
        48048128, '995561a68c8c5b142f13f1967730ce8c08d4f0db9574db8e987d915f2b118dcc'),
    'damnation': (23756800, '8b97ca7d73a4b720ba1b88deeb5a250b02a3f6886ffd2ddaa789cc9ad05e47b8',
        47968768, '87bd92c1821ff20bed03c559d7be5ac43baeb44aa68342ffdbb5a8cd978f711c'),
    'ratrace': (23840768, 'ad83896842b2303d145b944f644e4ddfe8d0dd80feda7efcb058a0f567a6bdec',
        47403008, '577886f949f2648f04cf99e354fb04ca08d7b9199636c2f9fe1a161ccdaa4b1e'),
    'prisoner': (21323776, '50740d89600b7fa53613ced764fa6e688434a8132948224658bf1ed07662aa3d',
        43974656, '85ff2fdc117b7640bda85a490b30c662f2626bd6d0685e41832935422891c307'),
    'hangemhigh': (21338112, '932a3a13a69fff5dd6e69c9b2c896c1bf3942c54a2a207975c4d0da4df07ed30',
        43700224, 'e3e4b25ed36178926f0df11f3cd82b07e2071f6aa0efaa77f0cfde8f8480981f'),
    'chillout': (20336640, '81819e9d7037a6b6c4e9a7472a431ebb53ed25ea00d8af634ac8384f1fe13d8b',
        41967104, '52bc3e23b9142cf479770d1d601fe08b13d78fe3eb05eca72d313b7ebbd21ffb'),
    'carousel': (20215808, '1c6353df6e4530437841b09775e62fa067caa46bb0e40f4579bbf2276991a38e',
        42367488, '3e4be29a7952af0b3f19a7540e454fd0deece6d6078c256cde70a734beba55e1'),
    'boardingaction': (22075392, 'e14cebccd63f7780e1f5da7a1687a37bdaa3f0821701095075336ef548889efb',
        45346304, '34ddf961973b6aaf05f40f26bd9ad536ca97f8af503995611535ccf6ce911522'),
    'bloodgulch': (22571008, '50fe52406f075d975e24100a65b26ff696458023dd3509878953052ab0ef858f',
        44328960, '9eb0374cbe4571af640ae0d2e0ebf6119543cdbbece44d49543b4748c4cf0dc1'),
    'wizard': (19687424, 'e782ad19071efa1c73f30be44feb6b7e073e15f00337e23d877a8e9776f2f3ad',
        40681984, '978539ff70085998485870a15d4ec81d3811442ac48f9265db668a396f6ff1eb'),
    'putput': (19777536, '7f5d13335d44982b0fbf7aad6b767d035aa64b32bf5c38d6d29bf20916fce5d4',
        40668672, 'efbe723188f34053d318105f20c7046f263171fbb937fbad45e3d00f8437f243'),
    'longest': (20512768, '12977a4042ecb960cbe48ef936046a35c6754c18829ad2d17206b66ddd03864e',
        42141696, 'e045b0843f4a167a516f050161a651610fafc94ccc8c5f2c5d5725d108d5d1b9'),
}


class BuildError(ValueError):
    pass


def sha256_file(path, chunk=1 << 20):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(chunk):
            digest.update(block)
    return digest.hexdigest()


def identify(name, size, sha256):
    """'disc' or 'expanded' if this is the known map of that name, else BuildError."""
    if name not in MAPS:
        raise BuildError(f'{name}.map is not one of build {BUILD}\'s {len(MAPS)} maps')
    disc_size, disc_hash, expanded_size, expanded_hash = MAPS[name]
    if (size, sha256) == (disc_size, disc_hash):
        return 'disc'
    if (size, sha256) == (expanded_size, expanded_hash):
        return 'expanded'
    raise BuildError(f'{name}.map ({size} bytes, sha256 {sha256[:12]}...) is not build {BUILD}\'s: '
                     'another release, a modified map or a bad dump')


def expand(raw):
    """A map as the Xbox reads it: the header as it is, the rest inflated. A map
    already expanded comes back as it is."""
    file_length = int.from_bytes(raw[8:12], 'little')
    if len(raw) == file_length:
        return raw
    body = zlib.decompress(raw[HEADER_SIZE:])
    expanded = raw[:HEADER_SIZE] + body
    if len(expanded) != file_length:
        raise BuildError(f'the map expands to {len(expanded)} bytes; its header says {file_length}')
    return expanded


def check_expanded(name, expanded):
    if name in MAPS and hashlib.sha256(expanded).hexdigest() != MAPS[name][3]:
        raise BuildError(f'{name}.map does not expand to build {BUILD}\'s')
