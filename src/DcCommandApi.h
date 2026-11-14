/*
 * mod-dungeon-clear — DcCommandApi.h
 *
 * What the `dc` chat commands take, for a module that sends them to a run's
 * tank itself -- PlayerbotAI::DoSpecificAction("dc skip", Event("dc", param,
 * issuer), true), as mod-ollama-chat does to steer a run from what a player
 * types in party. The actions authorize the issuer and answer in the addon's
 * chat line exactly as for a typed command. A module that uses this without
 * depending on this module:
 *
 *     #if __has_include("DcCommandApi.h")
 *     #include "DcCommandApi.h"
 *     #endif
 *     #if defined(DC_COMMAND_API) && DC_COMMAND_API >= 1
 *     ...
 *
 * 1: `dc skip <boss>` skips that boss wherever it is in the run, not only the
 *    one it is heading for, and `dc unskip <boss>` puts a skipped one back in
 *    its place in the order. <boss> is its creature entry, or part of its
 *    name. A bare `dc skip` is as it always was.
 *
 *    Older than that, and counted on as part of it: `dc pull off|on|dynamic`
 *    sets how the tank takes trash -- off is Leeroy (straight into each pack,
 *    fought where it stands), on is Advanced (every pack pulled back to a
 *    camp), dynamic decides pack by pack (the default). Sent to the leading
 *    tank, which is the only one that keeps it, during a run or before one
 *    (`dc on` starts the run that way); it reads back from the tank's
 *    DcKey::PullSetting (0 off, 1 on, 2 dynamic), and goes back to dynamic
 *    when the run ends however it ends. mod-ollama-chat sends it for "be
 *    more aggressive" and "slow down" in party.
 */

#ifndef _DC_COMMAND_API_H
#define _DC_COMMAND_API_H

#define DC_COMMAND_API 1

#endif  // _DC_COMMAND_API_H
