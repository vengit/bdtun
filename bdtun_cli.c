#include <stdio.h>

#include "bdtun.h"

void usage() {
    printf("Usage bdtun <command> PARAMETERS\n\n");
    printf("bdtun create <name> <blocksize> <size in blocks>\n");
    printf("bdtun resize <name> <new blocksize> <new size in blocks>\n");
    printf("bdtun remove <name>\n");
    printf("bdtun info <name>\n");
    printf("bdtun list\n");
}

int main(int char, char **args) {
    // TODO: grab some nice argument parser lib
}
