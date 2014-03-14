clean:
	rm -rf build proto/*.[ch]

.DEFAULT_GOAL = all
.PHONY : proto

proto: proto/*.proto
	protoc-c --proto_path proto --c_out proto proto/*.proto

CCFLAGS := $(shell pkg-config --cflags libprotobuf-c) -Werror
LIBS := $(shell pkg-config --libs fuse libprotobuf-c)

all:
	mkdir -p build
	$(CC) -o build/fuse-dfs-proto proto/*.c src/*.c -I. $(CCFLAGS) $(LIBS)
