#ifndef _UTILS_H
#define _UTILS_H

#include <fstream>
#include <open62541.h>

UA_Boolean PrintNode(UA_Client *pClient, const UA_NodeId &Id,
                     std::ofstream &out);

#endif // _UTILS_H
