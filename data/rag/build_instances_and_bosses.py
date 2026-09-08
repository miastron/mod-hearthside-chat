#!/usr/bin/env python3
"""Emit wow_instances.json and wow_bosses.json from a realm-verified table.

WHY THIS IS A SCRIPT AND NOT HAND-WRITTEN JSON
----------------------------------------------
Every proper noun below was read off the test realm, not recalled:

  * instance titles come from Map.dbc's name column (verified 2026-09-07:
    field 5, located by probing for map 0 == "Eastern Kingdoms")
  * level ranges come from LFGDungeons.dbc fields 21/22, targetLevelMin and
    targetLevelMax, cross-checked against the known Utgarde Keep 69-72
  * boss names come from acore_world.instance_encounters joined to
    creature_template, which is the server's own encounter roster

Keeping the table here, in one place, is what makes that provenance
checkable. Re-run this and then data/rag/generate_rag_sql.py after any edit.

THE TITLE IS AN ADDRESS, NOT A LABEL
------------------------------------
data/rag/README.md rule 6. hs_queue.cpp calls Hs_RagContextForKeys with
req.topicGate.instanceName, which hs_event.cpp/hs_handler.cpp fill from
Map::GetMapName(). So an instance entry's title MUST be the Map.dbc string
or the keyed lookup silently misses while scored retrieval still works --
a failure nothing in the harness would catch.

That string is frequently NOT what a player says. Verified divergences:

    Map.dbc                                 players say
    Hellfire Citadel: Ramparts              ramparts, hfr
    Coilfang: The Slave Pens                slave pens, sp
    Auchindoun: Shadow Labyrinth            shadow lab, slabs
    Tempest Keep: The Arcatraz              arcatraz, arc
    Opening of the Dark Portal              black morass, bm
    Magister's Terrace                      magisters terrace, mgt
    Violet Hold                             (NOT "The Violet Hold")

Hence: title = the Map.dbc address, keywords = what players call it.
"""

import io
import json
import pathlib

OUT = pathlib.Path(__file__).resolve().parent


def slug(s):
    keep = "".join(c.lower() if c.isalnum() else " " for c in s)
    return "_".join(keep.split())


# ---------------------------------------------------------------------------
# INSTANCES
#   title      -- Map.dbc name, verbatim. The keyed-lookup address.
#   lo, hi     -- LFGDungeons.dbc target level range; None for raids not in LFG
#   kind       -- prose fragment naming the format
#   where      -- the zone, for "where is X"
#   flavour    -- one hand-written distinguishing sentence
#   bosses     -- instance_encounters roster, in encounter order
#   words      -- what players call it, for scored retrieval
# ---------------------------------------------------------------------------
INSTANCES = [
    # --- Wrath 5-mans -----------------------------------------------------
    ("Utgarde Keep", 69, 72, "five-player dungeon", "Howling Fjord",
     "It is the first dungeon most players see in Northrend, a vrykul hold built into the cliffs above Daggercap Bay.",
     ["Prince Keleseth", "Dalronn the Controller", "Ingvar the Plunderer"],
     ["utgarde keep", "uk", "utgarde"]),
    ("The Nexus", 71, 73, "five-player dungeon", "Coldarra, in the Borean Tundra",
     "It is the middle ring of Malygos's spire, and the whole place is frozen blue crystal.",
     ["Commander Stoutbeard", "Grand Magus Telestra", "Anomalus", "Ormorok the Tree-Shaper", "Keristrasza"],
     ["the nexus", "nexus"]),
    ("Azjol-Nerub", 72, 74, "five-player dungeon", "Dragonblight",
     "It is a short three-boss run straight down through the old nerubian kingdom, and the second boss fight happens while you retreat up a web tunnel.",
     ["Krik'thir the Gatewatcher", "Hadronox", "Anub'arak"],
     ["azjol nerub", "azjol-nerub", "an", "nerub"]),
    ("Ahn'kahet: The Old Kingdom", 73, 75, "five-player dungeon", "Dragonblight",
     "It shares an entrance area with Azjol-Nerub but runs deeper and darker, and Amanitar only appears on heroic difficulty.",
     ["Elder Nadox", "Prince Taldaram", "Jedoga Shadowseeker", "Amanitar", "Herald Volazj"],
     ["ahnkahet", "the old kingdom", "old kingdom", "ok"]),
    ("Drak'Tharon Keep", 74, 76, "five-player dungeon", "on the border between the Grizzly Hills and Zul'Drak",
     "It is a Drakkari troll fortress, and King Dred at the end is a devilsaur kept as a pet.",
     ["Trollgore", "Novos the Summoner", "King Dred"],
     ["draktharon keep", "draktharon", "dtk"]),
    ("Violet Hold", 75, 77, "five-player dungeon", "Dalaran",
     "It is a prison break rather than a crawl: you hold a room against timed waves while sealed cells open, and which mini-bosses you get is randomised each run.",
     ["Erekem", "Moragg", "Ichoron", "Xevozz", "Lavanthor", "Zuramat the Obliterator", "Cyanigosa"],
     ["violet hold", "vh"]),
    ("Gundrak", 76, 78, "five-player dungeon", "Zul'Drak",
     "It is where the Drakkari are butchering their own animal gods, and Eck the Ferocious is a bonus boss that only exists on heroic.",
     ["Slad'ran", "Moorabi", "Drakkari Colossus", "Gal'darah", "Eck the Ferocious"],
     ["gundrak", "gd"]),
    ("Halls of Stone", 77, 79, "five-player dungeon", "The Storm Peaks",
     "The middle of it is the Tribunal of Ages, an escort-and-defend event where Brann Bronzebeard reads titan records while you keep him alive.",
     ["Krystallus", "Maiden of Grief", "Sjonnir The Ironshaper"],
     ["halls of stone", "hos"]),
    ("Halls of Lightning", 79, 80, "five-player dungeon", "The Storm Peaks",
     "It sits above Halls of Stone in the same titan complex, and Loken at the end is a straight damage race against a stacking pulse.",
     ["General Bjarngrim", "Volkhan", "Ionar", "Loken"],
     ["halls of lightning", "hol"]),
    ("Utgarde Pinnacle", 79, 80, "five-player dungeon", "Howling Fjord",
     "It is the upper half of the same vrykul keep as Utgarde Keep but tuned for level 80, and Skadi is fought from the back of a drake.",
     ["Svala Sorrowgrave", "Gortok Palehoof", "Skadi the Ruthless", "King Ymiron"],
     ["utgarde pinnacle", "up", "pinnacle"]),
    ("The Oculus", 79, 80, "five-player dungeon", "Coldarra, in the Borean Tundra",
     "Most of it is fought from drake mounts in mid-air, which made it the least popular dungeon in the Dungeon Finder by a wide margin.",
     ["Drakos the Interrogator", "Varos Cloudstrider", "Mage-Lord Urom", "Ley-Guardian Eregos"],
     ["the oculus", "oculus", "occ"]),
    ("The Culling of Stratholme", 79, 80, "five-player dungeon", "the Caverns of Time in Tanaris",
     "It replays Arthas purging Stratholme, and beating the optional timed run rewards the Bronze Drake mount.",
     ["Meathook", "Salramm the Fleshcrafter", "Chrono-Lord Epoch", "Mal'Ganis"],
     ["culling of stratholme", "cos", "strat cot"]),
    ("Trial of the Champion", 79, 80, "five-player dungeon", "the Argent Tournament grounds in Icecrown",
     "It opens with a mounted jousting round using vehicle controls before turning into a normal fight.",
     ["The Grand Champions", "Eadric the Pure", "Argent Confessor Paletress", "The Black Knight"],
     ["trial of the champion", "toc5", "totc"]),
    ("The Forge of Souls", 79, 80, "five-player dungeon", "Icecrown Citadel",
     "It is the first of the three patch 3.3 dungeons that lead into Icecrown Citadel itself.",
     ["Bronjahm", "Devourer of Souls"],
     ["forge of souls", "fos"]),
    ("Pit of Saron", 79, 80, "five-player dungeon", "Icecrown Citadel",
     "The last stretch is a scripted escape with Jaina or Sylvanas clearing the way ahead of you.",
     ["Forgemaster Garfrost", "Ick", "Krick", "Scourgelord Tyrannus"],
     ["pit of saron", "pos"]),
    ("Halls of Reflection", 79, 80, "five-player dungeon", "Icecrown Citadel",
     "It is two bosses and then a fighting retreat from the Lich King himself, who cannot be killed here.",
     ["Falric", "Marwyn", "The Lich King"],
     ["halls of reflection", "hor"]),

    # --- Wrath raids ------------------------------------------------------
    ("Naxxramas", None, None, "ten- and twenty-five-player raid", "Dragonblight",
     "It was moved from the Eastern Plaguelands and retuned for level 80 as Wrath's entry raid; it is split into the Arachnid, Plague, Military and Construct quarters, which can be cleared in any order before Sapphiron and Kel'Thuzad.",
     ["Anub'Rekhan", "Grand Widow Faerlina", "Maexxna", "Noth the Plaguebringer", "Heigan the Unclean",
      "Loatheb", "Instructor Razuvious", "Gothik the Harvester", "The Four Horsemen", "Patchwerk",
      "Grobbulus", "Gluth", "Thaddius", "Sapphiron", "Kel'Thuzad"],
     ["naxxramas", "naxx"]),
    ("The Obsidian Sanctum", None, None, "ten- and twenty-five-player raid", "the Chamber of Aspects beneath Wyrmrest Temple in Dragonblight",
     "Leaving Sartharion's three drake lieutenants alive raises the difficulty and the loot, which is where the 'Sartharion plus three drakes' achievement runs come from.",
     ["Tenebron", "Shadron", "Vesperon", "Sartharion"],
     ["obsidian sanctum", "os", "sarth"]),
    ("The Eye of Eternity", None, None, "ten- and twenty-five-player raid", "Coldarra, in the Borean Tundra",
     "It is a single fight against Malygos that ends with the whole raid on drakes in a vehicle phase.",
     ["Malygos"],
     ["eye of eternity", "eoe", "malygos raid"]),
    ("Ulduar", None, None, "ten- and twenty-five-player raid", "The Storm Peaks",
     "It opens with a vehicle assault on Flame Leviathan, and most bosses have an optional harder 'hard mode' triggered in-fight rather than by a difficulty setting; Algalon is a limited-attempt bonus encounter.",
     ["Flame Leviathan", "Ignis the Furnace Master", "Razorscale", "XT-002 Deconstructor",
      "The Assembly of Iron", "Kologarn", "Auriaya", "Hodir", "Thorim", "Freya", "Mimiron",
      "General Vezax", "Yogg-Saron", "Algalon the Observer"],
     ["ulduar"]),
    ("Trial of the Crusader", None, None, "ten- and twenty-five-player raid", "the Argent Tournament grounds in Icecrown",
     "It is a single arena room with no trash at all, run as five staged encounters; the Heroic version is separately locked and limited by a shared attempt counter.",
     ["Northrend Beasts", "Lord Jaraxxus", "Faction Champions", "Twin Val'kyr", "Anub'arak"],
     ["trial of the crusader", "toc", "totc25"]),
    ("Icecrown Citadel", None, None, "ten- and twenty-five-player raid", "Icecrown",
     "It is the final raid of Wrath of the Lich King, released wing by wing over patch 3.3, and ends with the Lich King on the roof of the Frozen Throne.",
     ["Lord Marrowgar", "Lady Deathwhisper", "Gunship Battle", "Deathbringer Saurfang", "Festergut",
      "Rotface", "Professor Putricide", "Prince Valanar", "Blood-Queen Lana'thel",
      "Valithria Dreamwalker", "Sindragosa", "The Lich King"],
     ["icecrown citadel", "icc"]),
    ("Vault of Archavon", None, None, "ten- and twenty-five-player raid", "Wintergrasp",
     "Access is a PvP prize rather than a raid lockout: only the faction currently holding Wintergrasp can enter, and each boss drops a mix of PvP and PvE gear.",
     ["Archavon the Stone Watcher", "Emalon the Storm Watcher", "Koralon the Flame Watcher", "Toravon the Ice Watcher"],
     ["vault of archavon", "voa", "archavon"]),
    ("The Ruby Sanctum", None, None, "ten- and twenty-five-player raid", "the Chamber of Aspects beneath Wyrmrest Temple in Dragonblight",
     "It arrived in patch 3.3.5 as the last raid of the expansion, and Halion is fought in both a physical and a shadow version of the room at once.",
     ["Baltharus the Warborn", "General Zarithrian", "Saviana Ragefire", "Halion"],
     ["ruby sanctum", "rs", "halion"]),
    ("Onyxia's Lair", None, None, "ten- and twenty-five-player raid", "Dustwallow Marsh",
     "Originally a level 60 raid, it was retuned to level 80 in patch 3.2.2 for the game's fifth anniversary and drops current Wrath-tier loot.",
     ["Onyxia"],
     ["onyxias lair", "onyxia", "ony"]),

    # --- Burning Crusade 5-mans ------------------------------------------
    ("Hellfire Citadel: Ramparts", 59, 62, "five-player dungeon", "Hellfire Peninsula",
     "It is the first Outland dungeon most players run, and much of it is fought on open walkways rather than corridors.",
     ["Watchkeeper Gargolmar", "Omor the Unscarred", "Vazruden the Herald"],
     ["hellfire ramparts", "ramparts", "hfr"]),
    ("Hellfire Citadel: The Blood Furnace", 61, 63, "five-player dungeon", "Hellfire Peninsula",
     "It is where the Legion was mass-producing fel orcs, and the middle boss is fought through a sequence of gas-filled cells.",
     ["The Maker", "Broggok", "Keli'dan the Breaker"],
     ["blood furnace", "bf"]),
    ("Coilfang: The Slave Pens", 62, 64, "five-player dungeon", "Zangarmarsh",
     "It is the shallowest of the three Coilfang wings and the usual first stop for a Cenarion Expedition reputation grind.",
     ["Mennu the Betrayer", "Rokmar the Crackler", "Quagmirran"],
     ["slave pens", "sp"]),
    ("Coilfang: The Underbog", 63, 65, "five-player dungeon", "Zangarmarsh",
     "The Black Stalker at the bottom is a giant electrified spider that levitates the party.",
     ["Hungarfen", "Ghaz'an", "Swamplord Musel'ek", "The Black Stalker"],
     ["underbog", "ub"]),
    ("Auchindoun: Mana-Tombs", 64, 66, "five-player dungeon", "Terokkar Forest",
     "It is the Consortium's wing of Auchindoun, and the ethereal prince at the end is one of the easier Outland end bosses.",
     ["Pandemonius", "Tavarok", "Nexus-Prince Shaffar"],
     ["mana tombs", "mt"]),
    ("Auchindoun: Auchenai Crypts", 65, 67, "five-player dungeon", "Terokkar Forest",
     "It is full of undead draenei, and Shirrak the Dead Watcher slows casting for the whole room.",
     ["Shirrak the Dead Watcher", "Exarch Maladaar"],
     ["auchenai crypts", "crypts", "ac"]),
    # 68-70, not 66-68: LFGDungeons row 171 (The Black Morass) has targetLevelMin/Max
    # zeroed like every other pre-3.3 Burning Crusade dungeon, and the 66-68 that used to
    # sit here is row 170's -- The Escape From Durnholde, the *other* Caverns of Time
    # instance. Realm-checked 2026-09-08: dungeon_access_template says map 269 admits at
    # 66, and every Black Morass wave creature is level 70 (its three bosses are 72).
    ("Opening of the Dark Portal", 68, 70, "five-player dungeon", "the Caverns of Time in Tanaris",
     "Players usually call it the Black Morass. It is a timed defence of Medivh through eighteen portal waves at the moment the Dark Portal was first opened, and it was the attunement step for Karazhan. The Dark Portal itself stands in the Blasted Lands, and this instance is only a vision of its past.",
     ["Chrono Lord Deja", "Temporus", "Aeonus"],
     ["black morass", "bm", "opening of the dark portal", "cot2"]),
    ("Auchindoun: Sethekk Halls", 67, 68, "five-player dungeon", "Terokkar Forest",
     "The arakkoa wing of Auchindoun, and on heroic it holds Anzu, the raven god summoned by druids for their flight-form quest.",
     ["Darkweaver Syth", "Talon King Ikiss", "Anzu"],
     ["sethekk halls", "sethekk"]),
    ("Auchindoun: Shadow Labyrinth", 69, 70, "five-player dungeon", "Terokkar Forest",
     "It is the longest and hardest Auchindoun wing, and Murmur at the end is a sound elemental with a room-wide pull-and-detonate.",
     ["Ambassador Hellmaw", "Blackheart the Inciter", "Grandmaster Vorpil", "Murmur"],
     ["shadow labyrinth", "shadow lab", "slabs"]),
    ("Hellfire Citadel: The Shattered Halls", 69, 70, "five-player dungeon", "Hellfire Peninsula",
     "It has a reputation as the hardest of the Hellfire wings, and its heroic version runs on a timer to rescue prisoners.",
     ["Grand Warlock Nethekurse", "Blood Guard Porung", "Warbringer O'mrogg", "Warchief Kargath Bladefist"],
     ["shattered halls", "sh"]),
    ("Coilfang: The Steamvault", 69, 70, "five-player dungeon", "Zangarmarsh",
     "It is the deepest Coilfang wing and the last step of the old Karazhan-era Cenarion Expedition grind.",
     ["Hydromancer Thespia", "Mekgineer Steamrigger", "Warlord Kalithresh"],
     ["steamvault", "sv"]),
    ("Tempest Keep: The Mechanar", 69, 70, "five-player dungeon", "Netherstorm",
     "It is one of the three satellite wings of Tempest Keep, all mechanical, and the shortest of them.",
     ["Mechano-Lord Capacitus", "Nethermancer Sepethrea", "Pathaleon the Calculator"],
     ["mechanar", "mech"]),
    ("Tempest Keep: The Botanica", 69, 70, "five-player dungeon", "Netherstorm",
     "It is a greenhouse wing of Tempest Keep, and Warp Splinter at the end summons saplings that must be killed fast.",
     ["Commander Sarannis", "High Botanist Freywinn", "Thorngrin the Tender", "Laj", "Warp Splinter"],
     ["botanica", "bot"]),
    ("Tempest Keep: The Arcatraz", 69, 70, "five-player dungeon", "Netherstorm",
     "It is a prison wing holding things the naaru locked away, and it was the attunement step for the Black Temple.",
     ["Zereketh the Unbound", "Dalliah the Doomsayer", "Wrath-Scryer Soccothrates", "Harbinger Skyriss"],
     ["arcatraz", "arc"]),
    ("Magister's Terrace", 69, 70, "five-player dungeon", "the Isle of Quel'Danas",
     "It was added in patch 2.4 as the last Burning Crusade dungeon, and its heroic mode drops the Swift White Hawkstrider mount.",
     ["Selin Fireheart", "Vexallus", "Priestess Delrissa", "Kael'thas Sunstrider"],
     ["magisters terrace", "mgt", "magister's terrace"]),

    # --- Burning Crusade raids -------------------------------------------
    ("Karazhan", 70, 73, "ten-player raid", "Deadwind Pass",
     "It is Medivh's abandoned tower and the entry raid of the Burning Crusade, known for its chess event and an optional opera encounter that rotates between three scripts.",
     ["Attumen the Huntsman", "Moroes", "Maiden of Virtue", "Opera Event", "The Curator",
      "Terestian Illhoof", "Shade of Aran", "Netherspite", "Chess Event", "Prince Malchezaar", "Nightbane"],
     ["karazhan", "kara", "kz"]),
    ("Gruul's Lair", None, None, "twenty-five-player raid", "Blade's Edge Mountains",
     "Two bosses only: a five-ogre council fight and then Gruul himself, who grows in size and damage on a timer.",
     ["High King Maulgar", "Gruul the Dragonkiller"],
     ["gruuls lair", "gruul"]),
    ("Magtheridon's Lair", None, None, "twenty-five-player raid", "Hellfire Peninsula",
     "A single boss beneath Hellfire Citadel, fought while five channelers hold him in place.",
     ["Magtheridon"],
     ["magtheridons lair", "magtheridon", "mag"]),
    ("Coilfang: Serpentshrine Cavern", None, None, "twenty-five-player raid", "Zangarmarsh",
     "It is the raid wing of Coilfang Reservoir and ends with Lady Vashj, whose fight requires relaying tainted cores between phases.",
     ["Hydross the Unstable", "The Lurker Below", "Leotheras the Blind", "Fathom-Lord Karathress",
      "Morogrim Tidewalker", "Lady Vashj"],
     ["serpentshrine cavern", "ssc", "vashj"]),
    ("Tempest Keep", None, None, "twenty-five-player raid", "Netherstorm",
     "Usually called The Eye; it is the central spire of Tempest Keep and ends with Kael'thas Sunstrider in a long multi-phase fight.",
     ["Al'ar", "Void Reaver", "High Astromancer Solarian", "Kael'thas Sunstrider"],
     ["tempest keep", "the eye", "tk"]),
    ("The Battle for Mount Hyjal", None, None, "twenty-five-player raid", "the Caverns of Time in Tanaris",
     "It replays the battle at the World Tree as eight waves of trash between bosses, which makes it far more trash-heavy than any other raid of its tier.",
     ["Rage Winterchill", "Anetheron", "Kaz'rogal", "Azgalor", "Archimonde"],
     ["mount hyjal", "hyjal", "mh", "battle for mount hyjal"]),
    ("Black Temple", None, None, "twenty-five-player raid", "Shadowmoon Valley",
     "Illidan Stormrage's fortress and the intended final raid of the Burning Crusade before the Sunwell was added.",
     ["High Warlord Naj'entus", "Supremus", "Shade of Akama", "Teron Gorefiend", "Gurtogg Bloodboil",
      "Reliquary of Souls", "Mother Shahraz", "The Illidari Council", "Illidan Stormrage"],
     ["black temple", "bt", "illidan"]),
    ("Zul'Aman", 70, 73, "ten-player raid", "the Ghostlands",
     "Added in patch 2.3, it runs on a timer: clearing the four animal-god bosses fast enough rewards the Amani War Bear.",
     ["Akil'zon", "Nalorakk", "Jan'alai", "Halazzi", "Hex Lord Malacrass", "Zul'jin"],
     ["zulaman", "za"]),
    ("The Sunwell", None, None, "twenty-five-player raid", "the Isle of Quel'Danas",
     "Added in patch 2.4 as the final raid of the Burning Crusade, and its gear was the last tier before Wrath launched.",
     ["Kalecgos", "Brutallus", "Felmyst", "The Eredar Twins", "M'uru", "Kil'jaeden"],
     ["sunwell plateau", "sunwell", "swp"]),

    # --- Classic dungeons -------------------------------------------------
    ("Ragefire Chasm", 15, 16, "five-player dungeon", "Orgrimmar, through the Cleft of Shadow",
     "It is the lowest-level dungeon in the game and the only one inside a capital city.",
     ["Oggleflint", "Taragaman the Hungerer", "Jergosh the Invoker", "Bazzalan"],
     ["ragefire chasm", "rfc"]),
    ("Deadmines", 17, 20, "five-player dungeon", "Westfall",
     "It runs through a mine and out onto a pirate ship moored in a cavern, and Edwin VanCleef leads the Defias Brotherhood at the end.",
     ["Rhahk'Zor", "Sneed", "Gilnid", "Mr. Smite", "Cookie", "Captain Greenskin", "Edwin VanCleef"],
     ["deadmines", "vc", "the deadmines"]),
    ("Wailing Caverns", 17, 20, "five-player dungeon", "the Barrens",
     "It is a maze of twisting tunnels with no clear route, which is most of its reputation.",
     ["Lady Anacondra", "Lord Cobrahn", "Kresh", "Lord Pythas", "Skum", "Lord Serpentis",
      "Verdan the Everliving", "Mutanus the Devourer"],
     ["wailing caverns", "wc"]),
    ("Shadowfang Keep", 18, 21, "five-player dungeon", "Silverpine Forest",
     "It is a haunted worgen castle, and the renegade mage Archmage Arugal waits at the top.",
     ["Rethilgore", "Razorclaw the Butcher", "Baron Silverlaine", "Commander Springvale",
      "Odo the Blindwatcher", "Fenrus the Devourer", "Wolf Master Nandos", "Archmage Arugal"],
     ["shadowfang keep", "sfk"]),
    ("Blackfathom Deeps", 21, 24, "five-player dungeon", "Ashenvale",
     "A flooded night elf temple to an old naga-worshipped god, with a long underwater stretch.",
     ["Ghamoo-ra", "Lady Sarevess", "Gelihast", "Lorgus Jett", "Old Serra'kis", "Twilight Lord Kelris", "Aku'mai"],
     ["blackfathom deeps", "bfd"]),
    ("Stormwind Stockade", 22, 25, "five-player dungeon", "Stormwind City",
     "A short prison-riot dungeon under the Trade District, and one of the fastest runs in the game.",
     ["Targorr the Dread", "Kam Deepfury", "Hamhock", "Bazil Thredd", "Dextren Ward"],
     ["stockade", "stocks", "stormwind stockade"]),
    ("Razorfen Kraul", 24, 27, "five-player dungeon", "the Southern Barrens",
     "An outdoor quilboar warren built from thorns rather than an indoor dungeon.",
     ["Roogug", "Aggem Thorncurse", "Death Speaker Jargba", "Overlord Ramtusk", "Agathelos the Raging", "Charlga Razorflank"],
     ["razorfen kraul", "rfk"]),
    ("Gnomeregan", 25, 28, "five-player dungeon", "Dun Morogh",
     "The irradiated gnome capital, and Mekgineer Thermaplugg at the end is fought around a room of bomb-dispensing panels.",
     ["Grubbis", "Viscous Fallout", "Electrocutioner 6000", "Crowd Pummeler 9-60", "Mekgineer Thermaplugg"],
     ["gnomeregan", "gnomer"]),
    ("Scarlet Monastery", 29, 40, "five-player dungeon", "Tirisfal Glades",
     "It is four separate wings behind one entrance -- Graveyard, Library, Armory and Cathedral -- run at increasing levels rather than as one dungeon.",
     ["Interrogator Vishas", "Bloodmage Thalnos", "Houndmaster Loksey", "Arcanist Doan", "Herod",
      "High Inquisitor Fairbanks", "High Inquisitor Whitemane"],
     ["scarlet monastery", "sm", "smc"]),
    ("Razorfen Downs", 34, 37, "five-player dungeon", "the Southern Barrens",
     "The undead half of the quilboar warrens, ending with the lich Amnennar the Coldbringer.",
     ["Tuten'kash", "Mordresh Fire Eye", "Glutton", "Amnennar the Coldbringer"],
     ["razorfen downs", "rfd"]),
    ("Uldaman", 37, 40, "five-player dungeon", "the Badlands",
     "A titan vault, and the first place most players meet the dwarves' own origin story.",
     ["Revelosh", "Baelog", "Ironaya", "Ancient Stone Keeper", "Galgann Firehammer", "Grimlok", "Archaedas"],
     ["uldaman", "ulda"]),
    ("Maraudon", 41, 48, "five-player dungeon", "Desolace",
     "Split into orange, purple and pristine sections with separate entrances, ending at Princess Theradras.",
     ["Noxxion", "Razorlash", "Lord Vyletongue", "Celebras the Cursed", "Landslide",
      "Tinkerer Gizlock", "Rotgrip", "Princess Theradras"],
     ["maraudon", "mara"]),
    ("Zul'Farrak", 43, 46, "five-player dungeon", "Tanaris",
     "Best known for the pyramid steps, where a huge troll wave attacks at once, and for Gahz'rilla.",
     ["Hydromancer Velratha", "Antu'sul", "Theka the Martyr", "Witch Doctor Zum'rah",
      "Nekrum Gutchewer", "Chief Ukorz Sandscalp", "Gahz'rilla"],
     ["zulfarrak", "zf"]),
    ("Sunken Temple", 47, 50, "five-player dungeon", "the Swamp of Sorrows",
     "Properly the Temple of Atal'Hakkar; it is a confusing layered ruin and a long run for its level.",
     ["Atal'alarion", "Dreamscythe", "Weaver", "Jammal'an the Prophet", "Morphaz", "Hazzas", "Shade of Eranikus"],
     ["sunken temple", "st", "temple of atalhakkar"]),
    ("Blackrock Depths", 49, 56, "five-player dungeon", "Blackrock Mountain",
     "By far the largest dungeon in the game, with a bar, an arena and a prison inside it, ending at Emperor Dagran Thaurissan.",
     ["Lord Roccor", "High Interrogator Gerstahn", "Houndmaster Grebmar", "Pyromancer Loregrain",
      "Lord Incendius", "Warder Stilgiss", "Fineous Darkvire", "Bael'Gar", "General Angerforge",
      "Golem Lord Argelmach", "Ambassador Flamelash", "Magmus", "Emperor Dagran Thaurissan"],
     ["blackrock depths", "brd"]),
    ("Blackrock Spire", 57, 63, "five- and ten-player dungeon", "Blackrock Mountain",
     "Split into Lower and Upper halves sharing one entrance; Upper was originally a ten-player raid.",
     ["Highlord Omokk", "Shadow Hunter Vosh'gajin", "War Master Voone", "Mother Smolderweb",
      "Quartermaster Zigris", "Halycon", "Overlord Wyrmthalak", "Pyroguard Emberseer",
      "Warchief Rend Blackhand", "The Beast", "General Drakkisath"],
     ["blackrock spire", "lbrs", "ubrs"]),
    ("Dire Maul", 55, 60, "five-player dungeon", "Feralas",
     "Three separate wings -- East, West and North -- and the North wing has a well-known trick for looting the king's tribute without killing him.",
     ["Zevrim Thornhoof", "Hydrospawn", "Lethtendris", "Alzzin the Wildshaper",
      "Illyanna Ravenoak", "Magister Kalendris", "Immol'thar", "Tendris Warpwood",
      "Prince Tortheldrin", "Guard Mol'dar", "Stomper Kreeg", "Guard Fengus",
      "Guard Slip'kik", "Captain Kromcrush", "Cho'Rush the Observer", "King Gordok"],
     ["dire maul", "dm east", "dm west", "dm north"]),
    ("Scholomance", 58, 60, "five-player dungeon", "the Western Plaguelands",
     "A necromancy school inside Caer Darrow, ending with Darkmaster Gandling.",
     ["Jandice Barov", "Rattlegore", "Marduk Blackpool", "Vectus", "Ras Frostwhisper",
      "Instructor Malicia", "Doctor Theolen Krastinov", "Lorekeeper Polkelt", "The Ravenian",
      "Lord Alexei Barov", "Lady Illucia Barov", "Darkmaster Gandling"],
     ["scholomance", "scholo"]),
    ("Stratholme", 58, 60, "five-player dungeon", "the Eastern Plaguelands",
     "Two separate entrances: the Scarlet side and the undead side, where Baron Rivendare drops the Deathcharger's Reins on a timed run.",
     ["The Unforgiven", "Hearthsinger Forresten", "Timmy the Cruel", "Cannon Master Willey",
      "Archivist Galford", "Balnazzar", "Baroness Anastari", "Nerub'enkan", "Maleki the Pallid",
      "Ramstein the Gorger", "Magistrate Barthilas", "Baron Rivendare"],
     ["stratholme", "strat", "live side", "baron run"]),

    # --- Classic raids ----------------------------------------------------
    ("Molten Core", 60, 63, "forty-player raid", "Blackrock Mountain",
     "The game's first full raid tier, ten bosses ending at Ragnaros, and the source of Tier 1 armour.",
     ["Lucifron", "Magmadar", "Gehennas", "Garr", "Shazzrah", "Baron Geddon",
      "Sulfuron Harbinger", "Golemagg the Incinerator", "Majordomo Executus", "Ragnaros"],
     ["molten core", "mc"]),
    ("Blackwing Lair", 60, 63, "forty-player raid", "Blackrock Mountain",
     "Nefarian's lair above Blackrock Spire, and the source of Tier 2 armour.",
     ["Razorgore the Untamed", "Vaelastrasz the Corrupt", "Broodlord Lashlayer", "Firemaw",
      "Ebonroc", "Flamegor", "Chromaggus", "Nefarian"],
     ["blackwing lair", "bwl"]),
    ("Zul'Gurub", 57, 63, "twenty-player raid", "Stranglethorn Vale",
     "A troll city raid on a three-day lockout, ending at the blood god Hakkar.",
     ["High Priest Venoxis", "High Priestess Jeklik", "High Priestess Mar'li", "Bloodlord Mandokir",
      "Gahz'ranka", "High Priest Thekal", "High Priestess Arlokk", "Jin'do the Hexxer", "Hakkar"],
     ["zulgurub", "zg"]),
    ("Ruins of Ahn'Qiraj", 60, 63, "twenty-player raid", "Silithus",
     "The smaller of the two Ahn'Qiraj raids, opened by the same war effort as the Temple.",
     ["Kurinnaxx", "General Rajaxx", "Moam", "Buru the Gorger", "Ayamiss the Hunter", "Ossirian the Unscarred"],
     ["ruins of ahnqiraj", "aq20"]),
    ("Ahn'Qiraj Temple", 60, 63, "forty-player raid", "Silithus",
     "The larger Ahn'Qiraj raid, ending at C'Thun, and the source of Tier 2.5 armour.",
     ["The Prophet Skeram", "Battleguard Sartura", "Fankriss the Unyielding", "Viscidus",
      "Princess Huhuran", "Twin Emperors", "Ouro", "C'Thun"],
     ["temple of ahnqiraj", "aq40", "aq"]),
]


# ---------------------------------------------------------------------------
# BOSSES: (name, instance title, position, hand-written sentence)
# Names verified against acore_world.creature_template via instance_encounters.
# Scoped to Wrath plus the Classic/TBC end bosses players actually name --
# see this module's report and data/rag/README.md for what is deliberately
# left out.
# ---------------------------------------------------------------------------
BOSSES = [
    # Wrath 5-man end bosses
    ("Ingvar the Plunderer", "Utgarde Keep", "the final boss",
     "He is resurrected once by a val'kyr after the first kill, so the fight is effectively two rounds."),
    ("Prince Keleseth", "Utgarde Keep", "the first boss",
     "He freezes a player in an ice tomb that the rest of the group has to break."),
    ("Keristrasza", "The Nexus", "the final boss",
     "She applies a stacking chill that forces the group to keep moving for the whole fight."),
    ("Grand Magus Telestra", "The Nexus", "a boss",
     "She splits into three copies partway through, each with a different school of magic."),
    ("Anub'arak", "Azjol-Nerub", "the final boss",
     "He is the last nerubian king, raised by the Scourge; he also appears as the final encounter of Trial of the Crusader."),
    ("Hadronox", "Azjol-Nerub", "the second boss",
     "The fight is a fighting retreat up a web tunnel while she drags the party's own pursuers into it."),
    ("Krik'thir the Gatewatcher", "Azjol-Nerub", "the first boss",
     "He is guarded by three named watchers that can be pulled with him."),
    ("Herald Volazj", "Ahn'kahet: The Old Kingdom", "the final boss",
     "He casts an insanity effect that puts each player in a private phase fighting shadow copies of the group."),
    ("Prince Taldaram", "Ahn'kahet: The Old Kingdom", "a boss",
     "He is a san'layn vampire who vanishes and embraces a player for heavy damage until two nearby orbs are clicked."),
    ("Elder Nadox", "Ahn'kahet: The Old Kingdom", "the first boss",
     "He summons swarms of adds, and one guardian must be killed fast or he becomes immune."),
    ("King Dred", "Drak'Tharon Keep", "the final boss",
     "He is a devilsaur kept by the Drakkari, and the raptors around him can be pulled in with the fight."),
    ("Novos the Summoner", "Drak'Tharon Keep", "the second boss",
     "He is untouchable until four channelling crystals around the platform are destroyed."),
    ("Cyanigosa", "Violet Hold", "the final boss",
     "She arrives after the timed prison waves are survived, and is always the last encounter regardless of which mini-bosses spawned."),
    ("Gal'darah", "Gundrak", "the final boss",
     "He shifts between troll and rhino form, impaling players in one and goring in the other."),
    ("Moorabi", "Gundrak", "a boss",
     "He tries to transform into a mammoth, which has to be interrupted or the fight gets much harder."),
    ("Sjonnir The Ironshaper", "Halls of Stone", "the final boss",
     "He is reached after the Tribunal of Ages escort, and floods the room with iron and earthen adds."),
    ("Maiden of Grief", "Halls of Stone", "a boss",
     "She drops shadow pools that have to be moved out of quickly."),
    ("Loken", "Halls of Lightning", "the final boss",
     "The fight is a damage race against Lightning Nova and a pulse that hits harder the further away you stand."),
    ("General Bjarngrim", "Halls of Lightning", "the first boss",
     "He patrols with an escort and cycles through three combat stances."),
    ("King Ymiron", "Utgarde Pinnacle", "the final boss",
     "He is the vrykul king, and he channels power from the burial barrows around the room."),
    ("Skadi the Ruthless", "Utgarde Pinnacle", "a boss",
     "The first half is fought from a drake, throwing harpoons at his mount before he lands."),
    ("Svala Sorrowgrave", "Utgarde Pinnacle", "the first boss",
     "She sacrifices a player on an altar partway through and must be interrupted or healed past it."),
    ("Ley-Guardian Eregos", "The Oculus", "the final boss",
     "He is fought entirely from drake mounts, which is why the dungeon has the reputation it does."),
    ("Mal'Ganis", "The Culling of Stratholme", "the final boss",
     "He is the dreadlord who baits Arthas into the purge, and he escapes rather than dying."),
    ("Chrono-Lord Epoch", "The Culling of Stratholme", "a boss",
     "He is an infinite dragonflight agent trying to stop the purge from happening as it should."),
    ("The Black Knight", "Trial of the Champion", "the final boss",
     "He rises twice after being killed, first as a skeleton and then as a ghost."),
    ("Devourer of Souls", "The Forge of Souls", "the final boss",
     "It is a three-faced construct that alternates a mirrored fear with a wailing beam."),
    ("Bronjahm", "The Forge of Souls", "the first boss",
     "He is the Godfather of Souls, and at thirty percent health he stops and channels while soul fragments close on him."),
    ("Scourgelord Tyrannus", "Pit of Saron", "the final boss",
     "He is fought at the end of the scripted escape, after his frost wyrm Rimefang harries the group up the ramp."),
    ("Forgemaster Garfrost", "Pit of Saron", "the first boss",
     "He forges a weapon mid-fight, changing his abilities each time he picks a new one up."),
    ("Falric", "Halls of Reflection", "the first boss",
     "He appears after a set of scripted waves, and applies a stacking fear that grows through the fight."),
    ("Marwyn", "Halls of Reflection", "the second boss",
     "He is the second of the two captains fought before the Lich King chase begins."),

    # Wrath raid bosses
    ("Kel'Thuzad", "Naxxramas", "the final boss",
     "He is the lich who founded the Scourge's Cult of the Damned, fought at the top of the necropolis after Sapphiron."),
    ("Sapphiron", "Naxxramas", "the boss before Kel'Thuzad",
     "A frost wyrm fought in an ice-covered room where players hide behind ice blocks to survive its breath."),
    ("Patchwerk", "Naxxramas", "a boss",
     "A pure damage-and-healing check with no mechanics to speak of, long used as a benchmark fight."),
    ("Thaddius", "Naxxramas", "a boss",
     "Players are given positive or negative charges and have to split by sign or take heavy raid damage."),
    ("Heigan the Unclean", "Naxxramas", "a boss",
     "Known for its dance: the floor erupts in a repeating pattern the whole raid has to keep ahead of."),
    ("Loatheb", "Naxxramas", "a boss",
     "Healing is blocked except in brief windows, so healers rotate through a fixed order."),
    ("Instructor Razuvious", "Naxxramas", "a boss",
     "He is tanked by mind-controlled understudies rather than by a player."),
    ("Maexxna", "Naxxramas", "a boss",
     "She webs players to the wall, and the rest of the raid has to free them."),
    ("Anub'Rekhan", "Naxxramas", "the first boss of the Arachnid Quarter",
     "He is a nerubian crypt lord who summons corpse scarabs while chasing a random player."),
    ("Sartharion", "The Obsidian Sanctum", "the final boss",
     "Leaving his three drakes alive when he is pulled raises the difficulty and the reward."),
    ("Malygos", "The Eye of Eternity", "the final boss",
     "The Aspect of Magic, and the fight ends with the whole raid on drakes in a vehicle phase."),
    ("Yogg-Saron", "Ulduar", "the final boss",
     "An Old God fought in three phases, including a stretch inside visions of the past; leaving fewer of Sara's keepers active makes it much harder."),
    ("Flame Leviathan", "Ulduar", "the first boss",
     "The entire fight is fought from siege vehicles rather than on foot."),
    ("Mimiron", "Ulduar", "a boss",
     "A four-phase mechanical fight, and its hard mode sets the room on fire for the duration."),
    ("Thorim", "Ulduar", "a boss",
     "The raid splits in two, one group through an arena gauntlet and one up a corridor, meeting at the top."),
    ("Freya", "Ulduar", "a boss",
     "Her hard mode is set by leaving up to three elders alive when she is pulled."),
    # Hodir is deliberately absent, and it is a judgment call rather than an
    # oversight. "Hodir" is a one-word title, so any query containing the word
    # takes the full aboutness bonus -- which let this entry beat The Storm
    # Peaks on "where do i find the sons of hodir". The title cannot be
    # disambiguated: a boss entry has to be titled with the creature name or
    # hs_event.cpp's death trigger cannot retrieve it. The Sons of Hodir are a
    # daily faction every level-80 character grinds for shoulder enchants,
    # while Hodir is one boss inside a 25-man raid, so the faction question is
    # far more common in chat -- and answering it with an Ulduar encounter is
    # wrong, not merely partial.
    ("Kologarn", "Ulduar", "a boss",
     "A giant built into the wall, whose two arms are killed separately and regrow."),
    ("XT-002 Deconstructor", "Ulduar", "a boss",
     "Its heart is exposed periodically, and attacking it there is what triggers the hard mode."),
    ("Algalon the Observer", "Ulduar", "a bonus boss",
     "A limited-attempt encounter behind a hard-mode gate, added as the expansion's most exclusive kill at the time."),
    ("General Vezax", "Ulduar", "the boss before Yogg-Saron",
     "Mana regeneration is shut off for the fight, so casters draw from saronite vapours instead."),
    ("Lord Jaraxxus", "Trial of the Crusader", "a boss",
     "An eredar lord accidentally summoned into the arena partway through the event."),
    ("The Lich King", "Icecrown Citadel", "the final boss",
     "Arthas Menethil, fought on the roof of the Frozen Throne; it is the last encounter of Wrath of the Lich King. He also appears unkillable at the end of Halls of Reflection."),
    ("Sindragosa", "Icecrown Citadel", "the boss before the Lich King",
     "Sapphiron's risen mate, who stacks a frost debuff that forces players to hide behind ice tombs."),
    ("Professor Putricide", "Icecrown Citadel", "a boss",
     "The end of the plague wing, fought around slime pools he brews mid-fight."),
    ("Lord Marrowgar", "Icecrown Citadel", "the first boss",
     "He impales players on bone spikes and whirlwinds across the room."),
    ("Deathbringer Saurfang", "Icecrown Citadel", "a boss",
     "He gains power from blood drawn off the raid, so damage taken is what fuels him."),
    ("Lady Deathwhisper", "Icecrown Citadel", "the second boss",
     "She is shielded by mana until it is burned through, and raises adds from the cultists around her."),
    ("Blood-Queen Lana'thel", "Icecrown Citadel", "a boss",
     "She bites a player who must pass the bite on before a timer, spreading it through the raid."),
    ("Halion", "The Ruby Sanctum", "the final boss",
     "He is fought in a physical and a shadow version of the same room at once, with the raid split between them."),
    ("Archavon the Stone Watcher", "Vault of Archavon", "the first boss",
     "He is only reachable by the faction that currently holds Wintergrasp."),
    ("Onyxia", "Onyxia's Lair", "the only boss",
     "A black dragon fought in ground and air phases; retuned from level 60 to level 80 in patch 3.2.2."),

    # The Classic and TBC end bosses players actually name
    ("Ragnaros", "Molten Core", "the final boss",
     "A firelord summoned by the Dark Iron dwarves, and the game's first raid-ending boss."),
    ("Nefarian", "Blackwing Lair", "the final boss",
     "Deathwing's son, who calls out class-wide effects by name during the fight."),
    ("Hakkar", "Zul'Gurub", "the final boss",
     "The blood god of the Gurubashi trolls, weakened by killing his high priests first."),
    ("C'Thun", "Ahn'Qiraj Temple", "the final boss",
     "An Old God fought partly from inside its own stomach."),
    ("Edwin VanCleef", "Deadmines", "the final boss",
     "The leader of the Defias Brotherhood, fought on the deck of a ship inside the mine."),
    ("Archmage Arugal", "Shadowfang Keep", "the final boss",
     "The renegade mage who created the worgen infesting the keep."),
    ("Baron Rivendare", "Stratholme", "the final boss of the undead side",
     "He drops the Deathcharger's Reins, and a timed run of his wing rewards extra loot."),
    ("Darkmaster Gandling", "Scholomance", "the final boss",
     "The headmaster of the necromancy school, who teleports players into side rooms during the fight."),
    ("Emperor Dagran Thaurissan", "Blackrock Depths", "the final boss",
     "The Dark Iron emperor, fought beside his wife Moira, who must be left alive."),
    ("Illidan Stormrage", "Black Temple", "the final boss",
     "The Betrayer, and the intended final boss of the Burning Crusade before the Sunwell was added."),
    ("Kael'thas Sunstrider", "Tempest Keep", "the final boss",
     "The blood elf prince, fought in a long multi-phase encounter; a weakened version of him is also the final boss of Magister's Terrace."),
    ("Lady Vashj", "Coilfang: Serpentshrine Cavern", "the final boss",
     "A naga sea witch whose middle phase requires relaying tainted cores to break her shield."),
    ("Kil'jaeden", "The Sunwell", "the final boss",
     "The Burning Legion's second-in-command, and the last boss of the Burning Crusade."),
    ("Archimonde", "The Battle for Mount Hyjal", "the final boss",
     "Fought at the World Tree, where players use tears of the goddess to survive his air burst."),
    ("Prince Malchezaar", "Karazhan", "the final boss",
     "A eredar prince at the top of the tower, who rains infernals into the room."),
    ("Gruul the Dragonkiller", "Gruul's Lair", "the final boss",
     "He grows larger and hits harder on a timer, so the raid spreads out to avoid shatter damage."),
    ("Magtheridon", "Magtheridon's Lair", "the only boss",
     "A pit lord held in place by five channelers who must be killed together."),
    ("Zul'jin", "Zul'Aman", "the final boss",
     "The Amani warlord, who cycles through animal-god forms as his health drops."),
    ("Mekgineer Thermaplugg", "Gnomeregan", "the final boss",
     "He is fought around a room of bomb-dispensing panels that have to be closed."),
    ("Archaedas", "Uldaman", "the final boss",
     "A titan construct who animates the statues lining the room as adds."),
    ("Murmur", "Auchindoun: Shadow Labyrinth", "the final boss",
     "A sound elemental with a room-wide pull followed by a detonation players must run out of."),
    ("Chief Ukorz Sandscalp", "Zul'Farrak", "the final boss",
     "The Sandfury chief, reached after the pyramid steps event."),
    ("Princess Theradras", "Maraudon", "the final boss",
     "An earth elemental princess at the bottom of the pristine waters wing."),
    ("King Gordok", "Dire Maul", "the final boss of the north wing",
     "Killing him grants the ogre suit tribute run, which is why players clear the wing carefully."),
    ("General Drakkisath", "Blackrock Spire", "the final boss of the upper wing",
     "He guards the orb used for the old Onyxia attunement chain."),
    ("Mutanus the Devourer", "Wailing Caverns", "the final boss",
     "He appears at the end of the escort event that the whole dungeon leads up to."),
    ("Amnennar the Coldbringer", "Razorfen Downs", "the final boss",
     "A lich raising the quilboar dead, and the highest-level threat in the warrens."),
    ("Herod", "Scarlet Monastery", "the final boss of the Armory wing",
     "The Scarlet champion, who whirlwinds across the room and then flees to pull the training hall."),
    ("High Inquisitor Whitemane", "Scarlet Monastery", "the final boss of the Cathedral wing",
     "She resurrects Scarlet Commander Mograine once, so both have to be brought down together."),
]


RAID_LEVEL = {'Naxxramas': 80, 'The Obsidian Sanctum': 80, 'The Eye of Eternity': 80, 'Ulduar': 80, 'Trial of the Crusader': 80, 'Icecrown Citadel': 80, 'Vault of Archavon': 80, 'The Ruby Sanctum': 80, "Onyxia's Lair": 80, "Gruul's Lair": 70, "Magtheridon's Lair": 70, 'Coilfang: Serpentshrine Cavern': 70, 'Tempest Keep': 70, 'The Battle for Mount Hyjal': 70, 'Black Temple': 70, 'The Sunwell': 70}


def level_phrase(title, lo, hi, kind):
    """Raids have no LFGDungeons row, so lo/hi are None for them and the
    level has to come from RAID_LEVEL above. An earlier version guessed 80
    whenever lo was None, which quietly made every Burning Crusade raid --
    Black Temple, Sunwell, SSC -- claim to be level 80 content."""
    if lo and hi and lo != hi:
        return f"a level {lo} to {hi} {kind}"
    if lo:
        return f"a level {lo} {kind}"
    lvl = RAID_LEVEL.get(title)
    if lvl is None:
        raise SystemExit(f"no level known for {title!r}; add it to RAID_LEVEL")
    return f"a level {lvl} {kind}"



# ---------------------------------------------------------------------------
# EXTRA_KEYWORDS: handles recovered from wow_dungeons_raids.json when it was
# deleted on 2026-09-07.
#
# That file's 21 topics are all covered here, but its keyword lists were not,
# and an audit found ~40 substantive handles with no counterpart -- including
# "mine cart", which data/rag/README.md rule 3 holds up as the example of a
# good multi-word keyword, and "van cleef", which players type as two words
# while the creature is "Edwin VanCleef" (one token after normalisation).
#
# Only handles that pass rule 2 are here: someone typing this term should
# plausibly want *this* paragraph. Deliberately NOT recovered:
#   * filler that sat on a dozen entries at once -- loot, experience, complex,
#     endgame, large, coordination. IDF already discounts them to nothing and
#     they dilute the specificity bonus of whatever holds them.
#   * every "level NN" keyword, nearly all of which disagreed with
#     LFGDungeons.dbc (Naxxramas carried "level 60"; it is 80 in Wrath).
#   * wrong facts -- Shadowfang Keep's "lord godfrey/baron ashbury/lord walden"
#     are the Cataclysm roster, and "thunderfury" was on Onyxia when it is a
#     Molten Core legendary.
#   * anything already reachable through the entry's own content or roster,
#     which retrieves at content weight and needs no duplicate handle.
EXTRA_KEYWORDS = {
    "Deadmines":            ["mine cart", "van cleef", "defias"],
    "Wailing Caverns":      ["naralex", "emerald dream"],
    "Blackfathom Deeps":    ["twilight hammer"],
    "Scholomance":          ["kirtonos", "scourge", "caer darrow"],
    "Stratholme":           ["scarlet crusade", "scourge", "plague"],
    "Onyxia's Lair":        ["black dragon", "dragonmurk"],
    "Molten Core":          ["fire elementals"],
    "Blackwing Lair":       ["black dragonflight", "drakes"],
    "Naxxramas":            ["scourge", "necropolis"],
    "Ulduar":               ["titan", "siege of ulduar", "inner sanctum"],
    "Blackrock Depths":     ["dark iron", "dwarves"],
    "Blackrock Spire":      ["burning steppes", "dragonkin"],
    "Dire Maul":            ["warpwood quarter", "capital gardens", "gordok commons", "ogres", "pusillin"],
    "Maraudon":             ["centaur", "valley of spears"],
    "Sunken Temple":        ["atal'ai", "avatar of hakkar"],
    "Zul'Farrak":           ["sandfury", "antusul"],
    "Zul'Gurub":            ["gurubashi"],
    "Ruins of Ahn'Qiraj":   ["qiraji", "silithid"],
    "Ahn'Qiraj Temple":     ["qiraji", "silithid"],
}

def instance_entry(title, lo, hi, kind, where, flavour, bosses, words):
    content = f"{title} is {level_phrase(title, lo, hi, kind)} in {where}. {flavour}"
    if bosses:
        if len(bosses) == 1:
            content += f" Its boss is {bosses[0]}."
        else:
            content += " Its bosses are " + ", ".join(bosses[:-1]) + f", and {bosses[-1]}."
    kws = list(dict.fromkeys(
        w.lower() for w in list(words) + EXTRA_KEYWORDS.get(title, [])))
    return {
        "id": "instance_" + slug(title),
        "title": title,
        "content": content,
        "keywords": kws,
        "tags": ["instance", "dungeon" if "five-player" in kind else "raid"],
    }


def boss_entry(name, instance, position, note):
    inst = next(i for i in INSTANCES if i[0] == instance)
    lo, hi, kind, where = inst[1], inst[2], inst[3], inst[4]
    lvl = level_phrase(instance, lo, hi, kind)
    content = f"{name} is {position} of {instance}, {lvl} in {where}. {note}"
    return {
        "id": "boss_" + slug(name),
        "title": name,
        "content": content,
        "keywords": [name.lower(), instance.lower()],
        "tags": ["boss", "dungeon" if "five-player" in kind else "raid"],
    }


def main():
    instances = [instance_entry(*i) for i in INSTANCES]
    bosses = [boss_entry(*b) for b in BOSSES]

    ids = [e["id"] for e in instances + bosses]
    dupes = {i for i in ids if ids.count(i) > 1}
    if dupes:
        raise SystemExit(f"duplicate ids: {sorted(dupes)}")

    for name, path in (("wow_instances.json", instances), ("wow_bosses.json", bosses)):
        io.open(OUT / name, "w", encoding="utf-8").write(
            json.dumps(path, indent=2, ensure_ascii=False) + "\n")
        print(f"{len(path):>4} entries -> data/rag/{name}")


if __name__ == "__main__":
    main()
