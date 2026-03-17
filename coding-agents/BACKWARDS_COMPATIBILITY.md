We aren't 100% beholden to backwards compatibility, but we have to weigh carefully the benefit versus the impact. We managed to get a huge benefit from multi-threading the unit movement and collision code without much, if any, impact on the games (ignoring bugs that were fixed). Though trying to do the same with Unit::Update, Unit::SlowUpdate or projectiles is a different matter: the impacts would be huge and there isn't really much that can be done about it. It is really about navigating the territory we've been put into. (edited)Monday, March 3, 2025 at 12:39 PM

Games are either mature and need stability, or they are in a more flexible development phase, but don't have the volunteer power to make their game and adjust to significant changes brought by engine updates.

This isn't Unreal or Unity, where games are made and finished and then new project started. For Recoil games, they are lifetime hobbies and projects - there isn't really new projects coming along to use the engine.

But we are not without opportunities for performance improvements.