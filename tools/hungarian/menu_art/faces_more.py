"""Faces of the screens after the main menu (select mode, result, mission failed, loading, game over, pause).
Imported at the end of faces.py; same conventions."""
from faces import FACES, Band, bands, face, ink_on_black, alpha_white, grey_on_dark

# extra prompt sources: the Options picture (FR has the e-acute of "sélectionner") and the library picture
for _l, (_a, _b) in [('EN', ('select', 'exit')), ('FR', ('sélectionner', 'quitter')), ('GE', ('wählen', 'beenden')),
                     ('IT', ('seleziona', 'esci')), ('SP', ('elegir', 'salir'))]:
    FACES['prompt'].bands += [Band('Options', 'Options', _l, (370, 362, 512, 388), _a),
                              Band('Options', 'Options', _l, (370, 388, 512, 412), _b)]
for _l, (_a, _b) in [('FR', ('sélectionner', 'quitter')), ('IT', ('seleziona', 'esci'))]:
    FACES['prompt'].bands += [Band('Story_Library', 'library', _l, (255, 150, 420, 175), _a),
                              Band('Story_Library', 'library', _l, (255, 175, 420, 200), _b)]

# the library menu (NEW / CONTINUE / LIBRARY / LAST STORY) is set in the main-menu face; FR gives Q
LIB_MENU = {'EN': ['NEW', 'CONTINUE', 'LIBRARY', 'LAST STORY'],
            'FR': ['NOUVEAU', 'CONTINUER', 'BIBLIOTHEQUE', 'DERNIERE HISTOIRE'],
            'GE': ['NEU', 'WEITER', 'BIBLIOTHEK', 'LETZTE STORY']}
LIB_ROWS = [(0, 21), (21, 42), (42, 63), (63, 84)]
for _l, _ts in LIB_MENU.items():
    FACES['menu_caps'].bands += [Band('Story_Library', 'library', _l, (270, y0, 512, y1), t)
                                 for (y0, y1), t in zip(LIB_ROWS, _ts)]

# ---------------------------------------------------------------------------------------------------------- select mode
face('select_title', 'SelectMode heading "select mode": big white grunge stencil (SelectMode_XX.txd selectmode, C8)',
     grey_on_dark(255, sat=40, floor=100), priority=4, seg='components', dp_split=True, min_letter_w=6,
     bands=bands('SelectMode', 'selectmode', {
         'EN': [((0, 376, 433, 437), 'select mode')], 'FR': [((0, 376, 433, 437), 'mode sélection')],
         'GE': [((0, 376, 433, 437), 'modus wählen')], 'IT': [((0, 376, 433, 437), 'modalità selezione')],
         'SP': [((0, 376, 433, 437), 'modo seleccionar')]}),
     notes='Grunge: letters touch through splatter and are cut by DP at the weakest columns (check the cut sheet).')
face('small_caps', 'SelectMode labels HERO / NORMAL / DARK / BOSS (SelectMode_XX.txd selectmode)',
     ink_on_black(255), outline={'rgb': (0, 0, 0)}, caps_only=True, priority=4, dp_split=True,
     bands=bands('SelectMode', 'selectmode', {
         l: [((0, 436, 80, 451), 'HERO'), ((0, 451, 86, 469), 'NORMALE' if l == 'IT' else 'NORMAL'),
             ((0, 469, 80, 487), 'DARK'), ((430, 385, 505, 408), 'BOSS')] for l in ('EN', 'FR', 'GE', 'IT', 'SP')}),
     notes='The same words in every language (HERO / NORMAL / DARK are mode names); IT adds NORMALE.')

# ---------------------------------------------------------------------------------------------------------- result
MC = {'EN': ('mission completed!', 'dark mission completed!', 'hero mission completed!', 'normal mission completed!'),
      'FR': ('mission terminée !', 'mission dark terminée !', 'mission hero terminée !', 'mission normale terminée !'),
      'GE': ('mission geschafft!', 'dark-mission geschafft!', 'hero-mission geschafft!', 'normal mission geschafft!'),
      'IT': ('missione completata!', 'missione dark completata!', 'missione hero completata!',
             'missione normale completata!'),
      'SP': ('¡misión superada!', '¡misión dark superada!', '¡misión hero superada!',
             '¡misión normal superada!')}
face('mission_done', 'Result screen "mission completed!" grunge face, grey fill + black/green-stained outline '
     '(Result_XX.gncp embedded 000_result, 512x512 C4)', ink_on_black(181), outline={'rgb': (0, 0, 0)},
     priority=5, seg='components', dp_split=True, min_letter_w=4,
     bands=bands('Result.gncp', None, {l: [((150, 90, 512, 146), a), ((145, 366, 512, 422), b),
                                           ((0, 422, 512, 468), c), ((0, 468, 512, 512), d)]
                                       for l, (a, b, c, d) in MC.items()}),
     notes='The outline carries green stains; it is regenerated as the plain most-common outline (stains lost).')
LABELS = {'EN': ['DARK SCORE:', 'HERO SCORE:', 'NORMAL SCORE:', 'TIME:', 'RINGS:', 'TIME BONUS:'],
          'FR': ['SCORE DARK:', 'SCORE HERO:', 'SCORE NORMALE:', 'TEMPS:', 'RINGS:', 'BONUS TEMPS:'],
          'GE': ['DARK-PUNKTESTAND:', 'HERO-PUNKTESTAND:', 'NORMAL-PUNKTESTAND:', 'ZEIT:', 'RINGS:', 'ZEIT-BONUS:'],
          'IT': ['PUNTEGGIO DARK:', 'PUNTEGGIO HERO:', 'PUNTEGGIO NORMALE:', 'TEMPO:', 'RINGS:', 'TEMPO BONUS:'],
          'SP': ['PUNTUACIÓN DARK:', 'PUNTUACIÓN HERO:', 'PUNTUACIÓN NORMAL:', 'TIEMPO:', 'RINGS:', 'TIEMPO EXTRA:']}
LABEL_ROWS = [(152, 172), (173, 193), (194, 215), (216, 236), (237, 257), (258, 278)]
face('result_labels', 'Result screen score labels DARK SCORE: ... (grey 206 fill, black outline, 10 px caps)',
     ink_on_black(206), outline={'rgb': (0, 0, 0)}, caps_only=True, priority=5, dp_split=True,
     exclude_langs=('GE',),
     bands=bands('Result.gncp', None, {l: [((150, y0, 338, y1), t) for (y0, y1), t in zip(LABEL_ROWS, ts)]
                                       for l, ts in LABELS.items()}),
     notes='GE sets these labels narrower (its letters are used only when no other language has them). '
           'TOTAL: (bold white) and RANK (own small face) are not mapped.')
MF = {'EN': ('dark mission:', 'hero mission:', 'failed'), 'FR': ('dark mission:', 'hero mission:', 'échoué'),
      'GE': ('dark-mission:', 'hero-mission:', 'gescheitert'), 'IT': ('missione dark:', 'missione hero:', 'fallito'),
      'SP': ('misión dark:', 'misión hero:', 'fallo')}
face('mission_failed', 'Mission failed: "dark mission: / failed" white bold rounded face, black outline '
     '(MissionFailed_XX.gncp embedded 000_missionfialed, 256x128 C4)', ink_on_black(255), outline={'rgb': (0, 0, 0)},
     priority=5, dp_split=True,
     bands=bands('MissionFailed.gncp', None, {l: [((0, 0, 256, 38), a), ((0, 38, 256, 78), b), ((0, 78, 256, 128), c)]
                                              for l, (a, b, c) in MF.items()}))
face('loading', 'Loading screen "loading" (Loading_XX.gncp embedded 000_loading, 128x256 C4): white on transparency',
     alpha_white, priority=5, exclude_langs=('FR',), dp_split=True,
     bands=bands('Loading.gncp', None, {'EN': [((0, 139, 128, 170), 'loading')],
                                        'FR': [((0, 139, 128, 170), 'chargement en cours')],
                                        'GE': [((0, 139, 128, 170), 'ladevorgang')],
                                        'IT': [((0, 139, 128, 170), 'caricamento')],
                                        'SP': [((0, 139, 128, 170), 'cargando')]}),
     notes='FR is set smaller (chargement en cours) and is used only for letters nobody else has.')

# ---------------------------------------------------------------------------------------------------------- game over
face('gameover_red', 'Game over heading "black out": red grunge, black outline (GameOver_XX.txd gameover, C4)',
     ink_on_black(206), outline={'rgb': (0, 0, 0)}, priority=5, seg='components', dp_split=True, min_letter_w=4,
     exclude_langs=('FR', 'IT', 'SP'),
     bands=bands('GameOver', 'gameover', {'EN': [((0, 0, 256, 61), 'black out')], 'GE': [((0, 0, 256, 61), 'black out')],
                                          'FR': [((0, 0, 256, 61), 'partie terminée')],
                                          'IT': [((0, 0, 256, 61), 'fine della partita')],
                                          'SP': [((0, 0, 256, 61), 'fin de la partida')]}),
     notes='FR/IT/SP squeeze the heading horizontally to fit; their letters are narrower than the EN/GE ones.')
face('continue_caps', 'Game over "CONTINUE?" white caps, black outline (GameOver_XX.txd gameover)',
     ink_on_black(255), outline={'rgb': (0, 0, 0)}, caps_only=True, priority=5, dp_split=True,
     bands=bands('GameOver', 'gameover', {'EN': [((0, 61, 256, 98), 'CONTINUE?')],
                                          'FR': [((0, 61, 256, 98), 'CONTINUER?')],
                                          'GE': [((0, 61, 256, 98), 'WEITER?')],
                                          'IT': [((0, 61, 256, 98), 'CONTINUARE?')],
                                          'SP': [((0, 61, 256, 98), '¿CONTINUAR?')]}))
face('yes_no', 'Game over "no / yes" white grunge, black outline (GameOver_XX.txd gameover)',
     ink_on_black(231), outline={'rgb': (0, 0, 0)}, priority=5, seg='components', dp_split=True, min_letter_w=4,
     bands=bands('GameOver', 'gameover', {'EN': [((0, 98, 256, 150), 'no yes')], 'FR': [((0, 98, 256, 150), 'non oui')],
                                          'GE': [((0, 98, 256, 150), 'nein ja')],
                                          'IT': [((0, 98, 256, 150), 'no sì')],
                                          'SP': [((0, 98, 256, 150), 'no sí')]}))

# ---------------------------------------------------------------------------------------------------------- pause
PAUSE = {'EN': ('restart', 'quit resume'), 'FR': ('redémarrer', 'quitter reprendre'), 'GE': ('neustart', 'beenden weiter'),
         'IT': ('ricomincia', 'abbandona continua'), 'SP': ('volver a empezar', 'abandonar continuar')}
face('pause_items', 'Pause menu items restart / quit / resume / back (pause/pauseNNNN_XX.txd pause_main, C8)',
     ink_on_black(255), outline={'rgb': (0, 0, 0)}, priority=5, dp_split=True, exclude_langs=('SP',),
     bands=bands('pause/pause0100', 'pause_main', dict(
         [(l, [((0, 97, 172, 126), a), ((0, 126, 256, 156), b)]) for l, (a, b) in PAUSE.items()]))
     + [Band('pause/pause0100', 'pause_main', 'EN', (0, 66, 82, 96), 'back')],
     notes='SP "volver a empezar" is set smaller; the back item has this size only in EN.')
PSMALL = {'EN': ('change mission', 'story route', None), 'FR': ('changer mission', 'histoire choisie', 'retour'),
          'GE': ('mission ändern', 'story-verlauf', 'zurück'),
          'IT': ('cambia missione', 'andamento della storia', 'indietro'),
          'SP': ('cambiar mision', 'hilo de la historia', 'atrás')}
face('pause_small', 'Pause menu small items change mission / story route (+ back in FR/GE/IT/SP)',
     ink_on_black(255), outline={'rgb': (0, 0, 0)}, priority=5, dp_split=True,
     bands=bands('pause/pause0100', 'pause_main', dict(
         [(l, [((100, 0, 222, 17), a), ((100, 17, 256, 37), b)] + ([((0, 66, 82, 96), c)] if c else []))
          for l, (a, b, c) in PSMALL.items()])))


# faces whose pictures have no capital letter in any language: only lowercase is needed (and can be built)
for _n in ('prompt', 'select_title', 'mission_done', 'mission_failed', 'loading', 'gameover_red', 'yes_no',
           'pause_items', 'pause_small'):
    FACES[_n].lower_only = True

# sample lines for the review sheets (draft wording only; the translation itself is the user's decision)
FACES['title_press'].samples = ('NYOMD MEG A START GOMBOT', 'NYOMJ STARTOT')
FACES['menu_caps'].samples = ('1 JÁTÉKOS', 'BEÁLLÍTÁSOK', 'TÖRTÉNET MÓD', 'SZAKÉRTŐ MÓD', 'ÚJ JÁTÉK', 'FOLYTATÁS',
                              'KÜLDETÉSVÁLASZTÓ')
FACES['prompt'].samples = ('kiválaszt', 'kilépés', 'vissza')
