[% INCLUDE $licence %]
/*
 *  * This code has been autogenerated, do not edit
 *   */

#ifndef [% module %][% name %]Msgs_H
#define [% module %][% name %]Msgs_H

[% FOREACH msg = msgs %]
#include "[% module %][% name %]Msg[% msg.cls_name %].h"
[%- END %]

#endif
