Import("env")

# CCFLAGS are emitted before CPPPATH. typeset/src is on CPPPATH and has its
# own TypesetBook.h, so this directory has to be searched first. The shared
# EPUB opener also includes ts_picture.h from here.
env.Append(CCFLAGS=["-I" + env.subst("$PROJECT_DIR") + "/lib/Typesetter"])
