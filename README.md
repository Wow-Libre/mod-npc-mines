# Campo de minas para AzerothCore

NPC de casino: rejilla 3x3, una bomba oculta. Destapas casillas y cobras cuando quieras. El premio en oro escala con casillas limpias; ademas se sortea un **item** validado contra `item_template`.

## Uso

1. Copia `mod-npc-mines` a `azerothcore/modules/`.
2. Compila.
3. Copia `mod_npc_mines.conf.dist` a configs.
4. Importa `data/sql/db-world/npc_mines.sql` en world.
5. `.npc add 1001257`

## Juego

- Elige apuesta.
- Destapa una casilla (A1-C3), como el tres en raya.
- Tesoro: el multiplicador sube. Puedes **Cobrar**.
- Bomba: pierdes la apuesta.

Al cobrar, el servidor:

1. Paga oro segun casillas limpias.
2. Elige un premio de la tabla `mines_prizes` (`min_revealed` <= casillas).
3. Consulta `item_template` por `entry` y comprueba `ItemTemplate` en memoria.
4. Si el id existe, entrega el item al jugador.

Edita `mines_prizes.item_entry` con ids reales de tu `item_template`.
