FUSE: Filesystem in Userspace for easy file concatenation of big files

# What does Concatfs do

Concatfs mounts a directory.
Files in this directory are concatenation description files.
Concatenation description consists of lines, each line consists of
byte offset, length, and filename parts separated by space.

Say there is a file 'vfile.dat' with content:

```
1024 512 /data/file1.dat
0 512 /data/file2.dat
```

Empty lines are ignored.

It manifests a readonly file on the same name 'vfile.dat', but its first 512
byte is got from file1.dat from offset 1024, the second 512 bytes from
file2.dat from the beginning. So vfile.dat is 1024 bytes long.

Make sure your constituting files are really at least that big in size,
that the prescribed length with the starting offset fits into it.
Otherwise you'll get unexpected data out.

Make sure referred files exist and readable. Relative paths are okay,
they are relative to the source directory.


# Dependency

You will need to install libfuse-dev to compile:

```
sudo apt-get install libfuse-dev
```

# Compile

```
gcc -Wall concatfs.c `pkg-config fuse --cflags --libs` -o concatfs
```

# Use

```
concatfs path-to-source-dir path-to-target-dir [fuse-mount options]
```
