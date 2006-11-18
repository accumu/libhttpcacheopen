
static const char cleanpathrcsid[] = /*Add RCS version string to binary */
        "$Id$";


static void cleanpath(char * path) {
    int         i, slash=0, lastc=0, slashoff=0;

    if(path[0] != '/') {
        return;
    }

    /* Kill multiple slashes */
    for(i=0; path[i]; i++) {
        if(path[i] == '/') {
            /* Kill '.' */
            if(path[i+1] == '.' && (!path[i+2] || path[i+2] == '/')) {
                path[i+1] = '/';
            }
            /* Kill '..' */
            if(i-slash == 3 && path[i-1] == '.' && path[i-2] == '.') {
                slashoff += 3;
                if(slashoff != i) {
                    slashoff++;
                    while(path[i-slashoff] != '/' && slashoff < i) {
                        slashoff++;
                    }
                }
            }
            if(slash+1 == i) {
                slashoff++;
                slash = i;
                continue;
            }
            else {
                slash = i;
            }
        }
        else {
            lastc = i;
        }
        path[i-slashoff] = path[i];
    }

    /* Position i before trailing \0 */
    i -= slashoff;
    i--;

    /* Kill trailing '..' */
    if(i>=2 && path[i] == '.' && path[i-1] == '.' && path[i-2] == '/') {
        i -= 2;
        if(i>0) {
            i--;
            while(i>0) {
                if(path[i] == '/') {
                    break;
                }
                i--;
            }
        }
    }

    /* Kill trailing slashes */
    for(; i>0; i--) {
        if(path[i] != '/') {
            break;
        }
    }

    /* i is positioned at the last valid character in string */
    path[i+1] = '\0';
}
