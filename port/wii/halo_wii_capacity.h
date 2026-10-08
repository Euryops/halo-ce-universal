/*
HALO_WII_CAPACITY.H

The Wii's counterpart of port/linux/include/halo_port_capacity.h, which
halo_wii_prefix.h puts in its place (this file takes its include guard). The
native builds size their pools for 128-player PC sessions, about 90 MB with
the caches; the Wii has 88 MB in all. So the Wii takes the Xbox's own pool
sizes (the values in parentheses in the native header), and its own places
for what the game wants at a fixed address. A Wii does not join a PC
session, so its sizes need not agree with theirs.

The fixed places, kept back from libogc's heap by port/wii/src/wii_main.c:

  game state  0x81200000-0x81800000  the top 6 MB of MEM1
  tag cache   0x90100000-0x91700000  22 MB at the foot of MEM2

The Xbox's maps are linked to a tag cache at 0x803A6000, where the Wii's
own code is. The map converter (stage 2) rebases them to 0x90100000.
*/

#ifndef __HALO_PORT_CAPACITY_H
#define __HALO_PORT_CAPACITY_H

/* ---------- fixed places */

#define HALO_WII_TAG_CACHE_BASE_ADDRESS 0x90100000
#define HALO_WII_TAG_CACHE_END 0x91700000

/* ---------- game state: the Xbox's 0x305000, with room for the larger
player and network tables the port keeps (halo_port_limits.h) */

#define HALO_PORT_GAME_STATE_BASE_ADDRESS 0x81200000
#define HALO_PORT_GAME_STATE_CPU_SIZE 0x5C0000
#define HALO_PORT_GAME_STATE_GPU_SIZE 0x40000
#define HALO_PORT_GAME_STATE_SIZE (HALO_PORT_GAME_STATE_CPU_SIZE+HALO_PORT_GAME_STATE_GPU_SIZE)

/* ---------- the Xbox's sizes */

#define HALO_PORT_MAXIMUM_ACTORS 256
#define HALO_PORT_MAXIMUM_PROPS 768
#define HALO_PORT_MAXIMUM_SWARMS 32
#define HALO_PORT_MAXIMUM_SWARM_COMPONENTS 256

#define HALO_PORT_TEXTURE_CACHE_PAGE_COUNT 0x580
#define HALO_PORT_TEXTURE_CACHE_SIZE (HALO_PORT_TEXTURE_CACHE_PAGE_COUNT*0x4000)

#define HALO_PORT_MAXIMUM_OBJECTS_PER_MAP 2048
#define HALO_PORT_OBJECT_MEMORY_POOL_SIZE 0x100000
#define HALO_PORT_MAXIMUM_CLUSTER_REFERENCES 2048
#define HALO_PORT_MAXIMUM_RENDERED_OBJECTS 256
#define HALO_PORT_MAXIMUM_CACHED_OBJECT_RENDER_STATES 256
#define HALO_PORT_MAXIMUM_AREA_OF_EFFECT_OBJECTS 64
#define HALO_PORT_MAXIMUM_LISTED_OBJECTS_PER_MAP 128

#define HALO_PORT_MAXIMUM_EFFECTS 256
#define HALO_PORT_MAXIMUM_EFFECT_LOCATIONS 512
#define HALO_PORT_MAXIMUM_PARTICLES 1024
#define HALO_PORT_MAXIMUM_PARTICLE_SYSTEMS 64
#define HALO_PORT_MAXIMUM_SYSTEM_PARTICLES 512
#define HALO_PORT_MAXIMUM_CONTRAILS 256
#define HALO_PORT_MAXIMUM_CONTRAIL_POINTS 1024
#define HALO_PORT_MAXIMUM_LIGHTS_PER_MAP 896
#define HALO_PORT_MAXIMUM_GAME_LOOPING_SOUNDS 1024

#endif /* __HALO_PORT_CAPACITY_H */
