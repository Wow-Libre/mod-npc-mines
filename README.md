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
2. Elige un `entry` al azar en `item_template` (`ORDER BY RAND()`).
3. La calidad sube con las casillas: 1-2 comun, 3-4 uncomun, 5-6 raro, 7-8 epico.
4. Comprueba que el ItemTemplate exista y te entrega el item.

No hace falta una tabla de premios fijos. Excluye quest, municion, keys y items DEPRECATED/test.
