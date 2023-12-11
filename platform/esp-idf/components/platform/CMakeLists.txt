idf_component_register(
    SRCS dummy.c vfs/vfs-esp.c
    INCLUDE_DIRS include wanted pub
    REQUIRES cwalk
    PRIV_REQUIRES vfs
    )
