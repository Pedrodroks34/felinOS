#include "usys.h"

int main(int argc, char **argv) {
    char *fast[] = { "count", "3", "150", 0 };
    char *slow[] = { "count", "3", "300", 0 };
    char *hog[]  = { "spin", 0 };
    int a = spawn("count", fast);
    int b = spawn("count", slow);
    int c = spawn("spin", hog);
    int status;

    puts("launch pid "); putnum(getpid());
    puts(" started "); putnum(a); puts(", "); putnum(b); puts(" and "); putnum(c); puts("\n");

    waitpid(a, &status);
    puts("launch: "); putnum(a); puts(" exited with "); putnum(status); puts("\n");
    waitpid(b, &status);
    puts("launch: "); putnum(b); puts(" exited with "); putnum(status); puts("\n");

    puts("launch: killing spinner "); putnum(c); puts("\n");
    kill(c, 9);
    waitpid(c, &status);
    puts("launch: "); putnum(c); puts(" exited with "); putnum(status); puts("\n");
    return 0;
}
