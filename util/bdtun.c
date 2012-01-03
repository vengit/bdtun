#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <argp.h>

#include "bdtun.h"

void usage() {
        printf(
                "Usage bdtun <command> PARAMETERS\n"
                "\n"
                "bdtun create <name> <blocksize> <size in blocks>\n"
                "bdtun resize <name> <new blocksize> <new size in blocks>\n"
                "bdtun remove <name>\n"
                "bdtun info <name>\n"
                "bdtun list\n"
        );
}

int open_ctrldev() {
        int f;
        f = open("/dev/bdtun", O_RDWR);
        if (f < 0) {
                printf("Could not open control device /dev/bdtun\n");
                // TODO: return values to constants
                exit(3);
        }
        return f;
}

int main(int argc, char **argv) {
        int f, ret, i;
        struct bdtun_info info;
        char **names;

        if (argc < 2) {
                usage();
                return 1;
        }
        if (strcmp(argv[1], "create") == 0) {
                if (argc != 5) {
                        usage();
                        return 1;
                }
                
                f = open_ctrldev();
                
                if ((ret = bdtun_create(f, argv[2], atoi(argv[3]), atoi(argv[4]))) < 0) {
                        printf("Operation failed\n");
                        PDEBUG("Return value was %d\n", ret);
                        return 4;
                }
                
                close(f);
                return 0;
        }
        if (strcmp(argv[1], "remove") == 0) {
                if (argc != 3) {
                        usage();
                        return 1;
                }
                f = open_ctrldev();
                if ((ret = bdtun_remove(f, argv[2])) < 0) {
                        printf("Operation failed\n");
                        PDEBUG("Return value was %d\n", ret);
                        return 4;
                }
                close(f);
                return 0;
        }
        if (strcmp(argv[1], "info") == 0) {
                if (argc != 3) {
                        usage();
                        return 1;
                }
                f = open_ctrldev();
                if ((ret = bdtun_info(f, argv[2], &info)) < 0) {
                        printf("Operation failed\n");
                        PDEBUG("Return value was %d\n", ret);
                        return 4;
                }
                printf(
                        "Information for device %s:\n\n"
                        "Size in bytes:      %" PRIu64 "\n"
                        "Block size:         %" PRIu64 "\n"
                        "Block device major: %d\n"
                        "Block device minor: %d\n"
                        "Char device major:  %d\n"
                        "Char device minor:  %d\n\n",
                        argv[2],
                        info.bd_size, info.bd_block_size,
                        info.bd_major, info.bd_minor,
                        info.ch_major, info.ch_minor                        
                );
                close(f);
                return 0;
        }
        if (strcmp(argv[1], "list") == 0) {
                if (argc != 2) {
                        usage();
                        return 1;
                }
                f = open_ctrldev();
                if ((ret = bdtun_list(f, 0, 32, &names)) < 0) {
                        printf("Operation failed\n");
                        PDEBUG("Return value was %d\n", ret);
                        return 4;
                }
                if (names[0] == NULL) {
                        printf("No tunnels found.\n");
                        return 0;
                }
                printf("The following tunnels were found:\n\n");
                i = 0;
                while (names[i] != NULL) {
                        printf("%s\n", names[i]);
                        i++;
                }
                close(f);
                return 0;
        }
        if (strcmp(argv[1], "resize") == 0) {
                if (argc != 4) {
                        usage();
                        return 1;
                }
                
                f = open_ctrldev();
                
                if ((ret = bdtun_resize(f, argv[2], atoi(argv[3]))) < 0) {
                        printf("Operation failed\n");
                        PDEBUG("Return value was %d\n", ret);
                        return 4;
                }
                
                close(f);
                return 0;
        }
        printf("No such command: %s\n", argv[1]);
        return 2;
}