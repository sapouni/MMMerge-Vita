#pragma once

#include <array>
#include <fstream>
#include <sstream>
#include <string>

namespace OpenYAMM::Game
{
// All guide copy remains text. Artwork contains no baked-in labels or instructions.
struct VitaGuideCopy
{
    struct Entry
    {
        std::string title;
        std::string body;
    };
    std::array<Entry, 17> entries{
        {{"Three worlds. One adventure.",
          "MMMerge brings Might and Magic VI, VII and VIII together. OpenYAMM recreates them in a shared engine "
          "inspired by Merge.\n\nChoose a starting world, create your party and explore. Other worlds remain available "
          "later.\n\nNew or returning? The Guide covers Vita controls, combat, potions and useful basics."},
         {"Your first adventure",
          "MMMerge connects Might and Magic VI, VII and VIII into one adventure. OpenYAMM recreates these games in a "
          "shared engine inspired by Merge. Some rules differ from playing the originals separately.\n\nMM6 begins in "
          "Enroth, MM7 in Antagarich, and MM8 in Jadame. You can visit the other worlds later with the same "
          "party.\n\nTalk to people, explore nearby areas and check your journal. Not every enemy is meant to be "
          "fought immediately. Retreat and return stronger."},
         {"Vita controls", "Left stick: move. Right stick: look. X: interact.\nSquare: attack. Circle: quick spell. "
                           "Triangle: jump.\nR: inventory. L: inspect the crosshair target.\nSelect: turn-based "
                           "combat. Start: game menu.\nD-pad shortcuts and L combinations can be changed in Vita Settings.\n\nIn menus, move the pointer with the left stick and click with "
                           "X, or use touch. In inventory, Circle inspects or applies a held item."},
         {"Turn-based combat",
          "Press Select to switch between real-time and turn-based combat. Turn-based combat gives you time to choose "
          "an action when a party member is ready.\n\nWatch the active character and combat phase: enemies also take "
          "turns, and movement is limited. Attack with Square or use a spell. During the movement phase, an attack can "
          "end movement first. Use Start for the game menu when taking a break."},
         {"Fighting and targeting",
          "Face your target with the crosshair. L inspects it; Square attacks. X is for interacting with doors, people "
          "and objects, not your dedicated attack button.\n\nWeapons and spells have recovery time: a character cannot "
          "act continuously. Use doorways and distance to avoid fighting a whole crowd at once. Look for useful items "
          "on defeated enemies."},
         {"Spells and quick spells",
          "Learn the required magic skill and spells. Open the spellbook from the HUD, choose a spell and follow its "
          "target prompt. Some spells need a party member or an item as target.\n\nSet a ready / quick spell in the "
          "spellbook: select a learned spell with X, then press Triangle to assign it. Circle inspects the spell; Start closes the book. It is set separately for each "
          "character. Double-clicking casts immediately instead. Use Circle in the world to cast the assigned spell. "
          "Check spell points, skill mastery and recovery time if "
          "casting fails."},
         {"Inventory and equipment",
          "Press R to open inventory. Select a character to manage their belongings. Use X to pick up and place an "
          "item; use the equipment slots on the character figure to equip it.\n\nWith empty hands, Circle inspects an "
          "item. Unidentified items hide details; broken equipment may need repair. A suitable skill or shop service "
          "can help."},
         {"Make a basic potion",
          "Put a reagent and an empty potion bottle in the same inventory.\n\n1. Pick up the reagent with X.\n2. Move "
          "the pointer over the empty bottle.\n3. Press Circle to mix them.\n\nThe ingredient is consumed and the "
          "bottle becomes a potion. Only potion reagents work: not every plant-like item is an ingredient."},
         {"Using and mixing items",
          "Pick up a potion and click the intended character's portrait or figure to use it. Inspect it first to learn "
          "its effects.\n\nCircle with an item held applies it to the inventory item under the pointer. Advanced "
          "potion mixtures require Alchemy and may fail or cause harm. Save before experiments. Keep useful bottles "
          "and read recipe notes."},
         {"Recruiting and followers",
          "Talk to people and look for Join or Hire topics. Adventurers can become playable party members; other "
          "followers offer services or bonuses. They are different kinds of help.\n\nRead the offer and any fee before "
          "accepting. Not everyone can be recruited. Check available adventurers at the Adventurers' Inn where "
          "offered."},
         {"Levels, skills and teachers",
          "Visit a training hall when you have enough experience to gain levels. Learn useful skills and spend skill "
          "points in the character screen.\n\nSkill level and mastery are different: seek teachers for Expert, Master "
          "and Grandmaster training where your class allows it. Promotions can unlock further progress. Read a "
          "teacher's requirements before spending points."},
         {"Health, conditions and rest",
          "Watch every party member's health, spell points and conditions. Someone who cannot act may be unconscious "
          "or affected by a condition.\n\nRest somewhere safe and keep food available. Nearby enemies can prevent "
          "resting. Potions, spells and temple services can help; different conditions need different cures. Do not "
          "assume one healing potion fixes everything."},
         {"Quests, maps and notes",
          "Read dialogue and check your quest journal for goals and clues. Use the HUD's map and journal controls to "
          "review where you are going.\n\nNotes and books may explain recipes, teachers or locations. A quest can "
          "require talking to the right person again after its main task. Keep quest items until you understand their "
          "purpose."},
         {"Shops, gold and banks",
          "Shops sell different kinds of goods and may identify or repair items. Selling spare equipment frees "
          "inventory space and earns gold.\n\nRead prices before buying, learning a skill or hiring help. Banks store "
          "gold and have deposit / withdraw dialogues. Use the Vita keyboard for an amount or save name. Confirm "
          "accepts your entry; Cancel backs out without applying it."},
         {"Doors, chests and traps",
          "Use the crosshair and X for doors, switches and containers. Look around carefully: some switches are small "
          "or control something elsewhere.\n\nChests can be trapped. Disarm Trap and other utility skills are worth "
          "learning, not just weapon skills. Watch your party's condition after opening a dangerous chest. Save before "
          "exploring an unfamiliar dungeon."},
         {"Travel and saving",
          "Coaches, boats, world exits and magic offer different ways to travel. Routes can depend on schedules or "
          "progress. Travelling to another world does not require starting a new party.\n\nUse Start to reach Save / "
          "Load. Keep several saves before major journeys and dangerous areas. Choose a slot carefully so one mistake "
          "does not replace your only safe save."},
         {"Taverns and Arcomage",
          "Visit taverns for conversation, services and, where offered, Arcomage. Read the local offer: not every "
          "tavern provides the same options.\n\nIn Arcomage, inspect cards and watch your resources, tower and wall. "
          "Cards can build your position or damage the opponent. Use the game's card text and controls rather than "
          "spending resources blindly."}}};

    VitaGuideCopy()
    {
        // Menu-open work only. Bounded read; incomplete edits retain the built-in entry.
        std::ifstream file("/data/vita-guide.txt", std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() > 65536)
            return;
        std::string text(static_cast<size_t>(file.tellg()), '\0');
        file.seekg(0);
        if (!file.read(text.data(), static_cast<std::streamsize>(text.size())))
            return;
        applyText(text);
    }

    void applyText(const std::string &text)
    {
        if (text.size() > 65536)
            return;
        std::istringstream input(text);
        std::string line, title, body;
        int index = -1;
        const auto commit = [&]()
        {
            while (!body.empty() && body.back() == '\n')
                body.pop_back();
            if (index < 0 || title.empty() || title.size() > 60 || body.empty() || body.size() > 4000)
                return;
            const auto printable = [](const std::string &value)
            {
                for (unsigned char c : value)
                    if (c != '\n' && (c < 32 || c > 126))
                        return false;
                return true;
            };
            if (printable(title) && printable(body))
                entries[index] = {title, body};
        };
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty() && line.front() == '[')
            {
                commit();
                index = -1;
                title.clear();
                body.clear();
                for (int i = 0; i < int(entries.size()); ++i)
                {
                    const std::string key = i == 0 ? "[intro]" : "[" + std::to_string(i) + "]";
                    if (line == key)
                    {
                        index = i;
                        break;
                    }
                }
            }
            else if (index >= 0)
            {
                if (title.empty())
                {
                    if (!line.empty())
                        title = line;
                }
                else
                {
                    body += line;
                    body += '\n';
                }
            }
        }
        commit();
    }
};
} // namespace OpenYAMM::Game
