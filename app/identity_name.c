#include "identity.h"
#include <string.h>
bool lora_name_valid(const char* name) {
    unsigned n = 0;
    if(!name) return false;
    for(; n <= LORA_NAME_MAX && name[n]; ++n) {
        /* ':' is the MeshCore name/message separator. */
        if((unsigned char)name[n] < 32 || (unsigned char)name[n] > 126 || name[n] == ':')
            return false;
    }
    return n && n <= LORA_NAME_MAX && name[0] != ' ' && name[n - 1] != ' ';
}
unsigned lora_mc_message_limit(const char* name) {
    return lora_name_valid(name) ? 43 - strlen(name) - 2 : 0;
}
