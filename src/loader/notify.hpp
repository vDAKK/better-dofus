#pragma once

// Windows notification that Dofus changed and no mod release fits it yet. Blocks
// while it is displayed: call it from its own thread. Takes no focus and touches
// no game process. Returns false if Windows refused it.
bool ShowIncompatibleGameNotice();
