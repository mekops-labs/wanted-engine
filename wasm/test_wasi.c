#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>

int main (void)
{
  DIR *dp;
  struct dirent *ep;
  dp = opendir ("./");

  if (dp != NULL)
  {
    while ((ep = readdir (dp))) {
      printf ("%s%c\n", ep->d_name, ep->d_type == DT_DIR ? '/' : ' ');
    }

    (void) closedir (dp);
  }
  else
    perror ("Couldn't open the directory");

  return 0;
}
