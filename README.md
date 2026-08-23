# mod-hearthside-chat

## Generator vs. reactive LLM endpoint

`HearthsideChat.LLM.*` (reactive replies) and `HearthsideChat.Generator.LLM.*` (idle-time corpus/
script authoring) are fully independent endpoint configs — separate `Url`/`Model`/`MaxTokens`/etc.
Reactive replies are latency-sensitive live chat; the generator runs offline against no player at
all. Because of that split, it's a legitimate and recommended deploy choice to point
`Generator.LLM.Url`/`Model` at a larger, slower model than the reactive endpoint — better line
quality costs nothing here since nothing is waiting on it. This is a compose-file/endpoint choice,
not something the module enforces or defaults.

## Required playerbots.conf settings

This module's reflex/corpus/inference chat replaces stock playerbot chat — running both together
double-talks. Before enabling `mod-hearthside-chat`, turn these `playerbots.conf` keys **off**:

- `AiPlayerbot.EnableBroadcasts`
- `RandomBotTalk`
- `RandomBotEmote`
- `RandomBotSuggestDungeons`
- `EnableGreet`
- `GuildFeedback`
- `RandomBotSayWithoutMaster`

The module does not check or warn about these at startup — it's the operator's responsibility to
disable them.