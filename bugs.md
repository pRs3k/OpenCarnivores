Below is a list of known bugs and their expected behaviors or workarounds.
Map / Radar

    The in-game map should show a red dot at the player's current location with a small green circle around the general area. (Check legacy code for the original circle radius.)
    If "Radar" is selected in MENU2, red dots should indicate the locations of dinosaurs that are selected in MENU2. Non-selected dinosaurs can still appear in the world but should not appear on the radar.

Graphics / Rendering

    Tree and ground models appear lower-resolution and possibly stretched compared to the original game. The world overall looks less sharp. In the original game, the world appeared sharper in "Software" video mode and more stretched in "Direct 3D." Direct3D should generally look better.
    When looking directly at the sun, the screen should receive an increasingly bright white/yellow filter effect.
    Our sun model has a much larger central circle radius than in the original.
    The sky renders correctly when looking straight up, but ripples/waves distort it progressively toward the horizon.
    Night mode currently shows a purely green screen after the hunt starts.
    After playing in night mode, switching to dawn or day retains the green filter from night mode; it should not.
    Some distant trees initially appear as tall bushes and then change to tree models when the player gets closer. The original had occasional similar behavior, but it should not occur here.

Player / World Interaction

    While the player is walking, the ground morphs under their feet (textures render but move unnaturally around the player); tree leaves exhibit the same behavior. This was present in the original game's software mode but not in Direct3D; it should not occur in our game.
    In hunts, ground dinosaurs are stuck in place (flying ones are fine). Even when scared, they run in place instead of moving around the world.
    When the player presses Escape > "R" to restart, the game should immediately reload the current world with the previous MENU2 settings; it should not return to MENU2.

UI / Text / Menus

    The text shown when leaving the world ("Preparing for evacuation...") is light blue but should be yellow.
    The text in MENU2 uses the wrong font and should align inside each row/cell. Disabled (greyed-out) text should be a slightly lighter grey than it is now.
    In the OPT menu:
        Brightness should not be settable to 0; limit the minimum to ~10%.
        The sliders use the wrong graphic.
        The font is incorrect.
        The unit label should read "US" instead of "imperial".
        Text is not center-aligned in each column; it should be.
    When in a hunt and pressing a number for a weapon slot where the weapon is unavailable, the text "No weapon" should appear in the bottom-left corner of the screen.

HUD / Trophy

    If the player clicks "trophy" in MENUQ, they should be taken to TROPHY.MAP with TROPHY_E.TGA always displayed at the top center of the screen while in TROPHY.MAP.
    When approaching a trophy dinosaur within a certain distance, the TROPHY.TGA graphic should appear in the bottom-right corner with the trophy text inside it.