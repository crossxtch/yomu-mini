Import("env")

# CCFLAGS are emitted before CPPPATH. Epub.cpp includes "ts_picture.h" from
# this directory, and that header includes the device PicturePage.h.
env.Append(CCFLAGS=["-I" + env.subst("$PROJECT_DIR") + "/lib/Typesetter"])
