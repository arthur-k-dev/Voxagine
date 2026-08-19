#pragma once

#include <cstdint>

class Entity;
class Player;
class World;

/* **A reference to one of the game's players that cannot go permanently
   stale.**

   Attachment used to be a serialized `Entity*` link: the level stores the
   player's entity id, `JsonSerializer::ResolveWorldLinks` retries it for
   `World::k_uiMaxWorldLinkRetries` PreTicks, and then gives up for good. That
   is a deadline race against chunk streaming, and it loses. Measured on
   `Fishing_Village_Beat1`, same binary and same command line, four headless
   runs: three of them abandoned

       Gave up connecting 'Player 1' on entity 13 to entity 12
       Gave up connecting 'Player 2' on entity 13 to entity 2955

   and left `CameraMultiplayer` with no players for the rest of the level - a
   camera parked over the world origin that never follows anything. The fourth
   run resolved and worked. Nothing about the level or the code differs between
   them; what differs is which frame the players' chunk finished admitting in.

   Two further ways the old shape went stale, both of which this closes:

   - **A resolved link is a raw `Entity*` and chunk streaming destroys players
     routinely.** The holder nulled its pointer from a `Destroyed` subscriber
     and never looked again, so one unload was permanent even though the player
     came back with the chunk.
   - **`GameManager` picked its players out of `FindEntitiesOfType<Player>()`
     by position**, which under streaming is admission order - so which player
     was P1 varied between runs, and an exact `size() == 2` test meant zero or
     three found silently attached nobody.

   So the identity is an *index*, not a pointer and not a discovery order, and
   resolution is a continuous query rather than a one-shot assignment. A slot
   that is empty tries again on every tick, forever, at the cost of one null
   test once it is filled. There is no deadline to lose. */
class PlayerSlot
{
public:
	/* Which player this slot wants. Serialized on `Player` as "Player Index";
	   see `Player::GetPlayerIndex`. */
	void SetIndex(int32_t iIndex) { m_iIndex = iIndex; }
	int32_t GetIndex() const { return m_iIndex; }

	/* Adopt an explicitly authored player - the serialized link, or the editor
	   assigning one by hand. Kept as an override so existing levels and the
	   inspector keep working unchanged, but it is no longer the *mechanism*:
	   whatever this sets, `Resolve` will re-find by index if it is ever lost.

	   Learns the index from the entity, so an authored link that names a player
	   the level did not index still re-resolves to the same one. */
	void Adopt(Entity* pEntity);

	/* The player, resolving from the world if this slot is empty. Returns null
	   only while no player with this index is in the world - which under
	   streaming is an ordinary, temporary state rather than a failure. */
	Player* Resolve(World* pWorld);

	/* The current pointer without resolving, for const readers. */
	Player* Peek() const { return m_pPlayer; }

	/* Forget the current player without forgetting which one it wants. Called
	   from the holder's `Destroyed` subscriber. */
	void Detach() { m_pPlayer = nullptr; }

private:
	Player* m_pPlayer = nullptr;
	int32_t m_iIndex = -1;
};

/* The one place that answers "which entity is player N". Scans the world's
   players for a matching `GetPlayerIndex()`. Linear in resident entities and
   only called while a slot is empty, so it costs nothing in steady state. */
Player* FindPlayerByIndex(World* pWorld, int32_t iIndex);
