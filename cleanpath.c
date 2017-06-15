/*
 * Copyright 2006-2017 Niklas Edmundsson <nikke@acc.umu.se>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


static const char cleanpathrcsid[] = /*Add RCS version string to binary */
        "$Id: cleanpath.c,v 1.2 2006/11/20 17:37:45 source Exp source $";


/* Clean an absolute path from //, .. and . */
static void cleanpath(char * path) {
    int         i;
    int         slash=0;        /* The position of the last '/' */
    int         rmchars=0;      /* How many characters we have removed */

    if(path[0] != '/') {
        return;
    }

    /* Do a single pass, copying characters within the string as we move
       along. When we stumble across single characters to remove we simply
       offset the source and destination index. */
    /* Kill multiple slashes */
    for(i=0; path[i]; i++) {
        if(path[i] == '/') {
            /* Kill '.' by replacing with '/', we remove multiple /:es */
            if(path[i+1] == '.' && (!path[i+2] || path[i+2] == '/')) {
                path[i+1] = '/';
            }
            /* Kill '..' by increasing the offset (ie. backing up in the
               already copied destination) until we have moved up a directory
               level or are at the root */
            if(i-slash == 3 && path[i-1] == '.' && path[i-2] == '.') {
                rmchars += 3;
                if(rmchars != i) {
                    /* We're not at the root, back up a level */
                    rmchars++;
                    while(path[i-rmchars] != '/' && rmchars < i) {
                        rmchars++;
                    }
                }
            }
            /* Kill // by incrementing offset while we see consecutive /:es */
            if(slash+1 == i) {
                rmchars++;
                slash = i;
                continue;
            }
            else {
                slash = i;
            }
        }
        if(rmchars > 0) {
            /* Only need to copy chars if we're offset */
            path[i-rmchars] = path[i];
        }
    }

    /* Position i before trailing \0 */
    i -= rmchars;
    i--;

    /* Kill trailing '..' */
    if(i>=2 && path[i] == '.' && path[i-1] == '.' && path[i-2] == '/') {
        i -= 2;
        if(i>0) {
            /* We're not at the root, back up a level */
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
