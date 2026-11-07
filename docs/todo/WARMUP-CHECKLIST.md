# Pipeline warm-up recording checklist

Goal: play every part of the game once with recording on, so `pipelines.warm` can be shipped (`pc/MACOS-PORT-PLAN.md`, item 4).

Map and section names come from the game files (`TdGame/CookedPC/Maps/SP00..SP09`). The chapter-to-map pairing is **inferred from map order and the wiki's chapter order**, not read from the game. A *section* is a streamed piece of a map (e.g. `Escape_Plaza`); every checkpoint starts in one, so visiting every section covers every checkpoint area.

Wiki checkpoint data was only partly readable (StrategyWiki and TrueAchievements returned 403). From ludo.guide: Prologue has checkpoints A-D, Chapter 1 has A-E, Chapter 2 has at least A-D. Chapters 3-9 are unknown: load them from the in-game Chapter select and tell me which you reached.

## Status (2026-11-07)

Recorded so far (by log, not complete): Prologue (`Edge_p`, `Edge_Pt2`), Chapter 1 start (`Escape_Intro`), part of Chapter 2 (`Stormdrain_*` assets opened), and SP09 `Scraper_*` via the saved-profile flow. Everything else is open. Unattended chapter loading did not work; play manually with `work/record-warm.sh`. The `work/warm-record/pipelines.warm` file (about 1,024 states) is not committed.

## How to record

`work/record-warm.sh` starts the game with timestamped logs. Note the clock time when you start each chapter/checkpoint. Then send me the times; I match them to `[HH:MM:SS]` lines (map loads in the runtime log `run.log`, `Pipeline recorded #N at HH:MM:SS` in the DXVK log).

## SP00: Training / Tutorial

Sections: 

Time Trial maps: `TT_TutorialA01_p`, `TT_TutorialA02_p`, `TT_TutorialA03_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## SP01: Prologue "The Edge" (Edge_*) and Ch1 "Flight" (Escape_*)

Sections: `Edge_Pt1`, `Edge_Pt2`, `Edge_SB01`, `Edge_SB02`, `Edge_SL01`, `Escape_Intro`, `Escape_Off`, `Escape_Plaza`, `Escape_R1`, `Escape_St1`

Time Trial maps: `TT_EdgeA01_p`, `TT_EscapeA01_p`, `TT_EscapeB01_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## SP02: Ch2 "Jacknife" (Stormdrain)

Sections: `Stormdrain_All`, `Stormdrain_Roof`, `Stormdrain_SB01`, `Stormdrain_SB02`, `Stormdrain_SL01`, `Stormdrain_Std`, `Stormdrain_StdE`, `Stormdrain_StdP`, `Stormdrain_boss`

Time Trial maps: `TT_StormdrainA02_p`, `TT_StormdrainB01_p`, `TT_StormdrainB02_p`, `TT_StormdrainB03_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## SP03: Ch3 "Heat" (Cranes)

Sections: `Cranes_Off`, `Cranes_Plaza`, `Cranes_Puzz`, `Cranes_Roof`, `Cranes_SB01`, `Cranes_SB02`, `Cranes_SL01`

Time Trial maps: `TT_CranesA01_p`, `TT_CranesB01_p`, `TT_CranesB02_p`, `TT_CranesC01_p`, `TT_CranesD01_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## SP04: Ch4 "Ropeburn" (Subway)

Sections: `Subway_Elev`, `Subway_EndFig`, `Subway_FanP`, `Subway_MoPu`, `Subway_NxtPlat`, `Subway_Plat`, `Subway_RbMiller`, `Subway_Ren`, `Subway_RenBuilding`, `Subway_RenCo`, `Subway_Roof`, `Subway_SB01`, `Subway_SB02`, `Subway_SL01`, `Subway_Stat`, `Subway_Train`, `Subway_TrainRide`, `Subway_Tunnel`

- [ ] story / checkpoints loaded: ______

## SP05: Ch5 "New Eden" (Mall)

Sections: `Mall_HW`, `Mall_Mall`, `Mall_MallExterior`, `Mall_MallLoadingBay`, `Mall_Music01`, `Mall_Music02`, `Mall_Music03`, `Mall_R1`, `Mall_R2`, `Mall_Roof`, `Mall_SB01`, `Mall_SB02`, `Mall_SL01`

Time Trial maps: `TT_MallA01_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## SP06: Ch6 "Pirandello Kruger" (Factory)

Sections: `Factory_Arena`, `Factory_Facto`, `Factory_Lbay`, `Factory_Pursu`, `Factory_Ren`, `Factory_Roof`, `Factory_SB01`, `Factory_SB02`, `Factory_SL01`

Time Trial maps: `TT_FactoryA01_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## SP07: Ch7 "The Boat"

Sections: `Boat_Bac`, `Boat_Chase`, `Boat_Cont`, `Boat_Deck`, `Boat_End`, `Boat_Harb`, `Boat_PDeck`, `Boat_SB01`, `Boat_SB02`, `Boat_SL01`

- [ ] story / checkpoints loaded: ______

## SP08: Ch8 "Kate" (Convoy)

Sections: `Convoy_Chase`, `Convoy_Conv`, `Convoy_Mall`, `Convoy_Roof`, `Convoy_SB01`, `Convoy_SB02`, `Convoy_SL01`, `Convoy_Snipe`

Time Trial maps: `TT_ConvoyA01_p`, `TT_ConvoyA02_p`, `TT_ConvoyB01_p`, `TT_ConvoyB02_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## SP09: Ch9 "The Shard" (Scraper)

Sections: `Scraper_Deck`, `Scraper_Duct`, `Scraper_Heli`, `Scraper_Lobby`, `Scraper_Main`, `Scraper_Mill`, `Scraper_OnlyTower`, `Scraper_Out`, `Scraper_Plaza`, `Scraper_Roof`, `Scraper_SB01`, `Scraper_SB02`, `Scraper_SL01`, `Scraper_Shaft`

Time Trial maps: `TT_ScraperA01_p`, `TT_ScraperB01_p`

- [ ] story / checkpoints loaded: ______
- [ ] time trials loaded: ______

## Also

- [ ] main menu, options, chapter select
- [ ] Speed Run / Time Trial menus
- [ ] pause menu, death and respawn, loading screens, cutscenes
- [ ] play with the settings you will ship (resolution and PhysX options change recorded render-target states)
