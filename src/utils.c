#include "utils.h"

/* Returns 1 only when the input string contains digits 0-9 exclusively. */
int is_numeric_str(const char *s) {
    size_t i;

    if (!s || !*s) {
        return 0;
    }

    for (i = 0; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
    }

    return 1;
}
