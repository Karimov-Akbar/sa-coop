Мод для GTA San Andreas 1.0 US, с которым сюжет можно пройти вдвоём по сети.

## Что нового

- **Бильярд вдвоём.** В баре на экране ставки нажмите U, и второй игрок сядет играть за ваш стол. Удары идут по очереди: второй смотрит на стол через вашу камеру, бьёт своими кнопками и держит кий в руке, а победителя называет сама игра.
- **Соревнования.** Баскетбол, бильярд, блэкджек, рулетку, колесо фортуны, танцы в клубе, встречу лоурайдеров, стадионы, гонки и триатлон можно пройти наперегонки: нажмите U в первые 20 секунд. Каждый играет у себя, а табло вверху экрана показывает счёт обоих, отсчёт до начала и крупно итог.
- **Подработки вдвоём по-настоящему.** В такси каждый возит своих пассажиров. В скорой, у пожарных и в полиции напарник получает свою машину и работает наравне с хостом. При ограблении домов оба выносят вещи в один фургон, а в работе сутенёра девушку может забрать любой из двоих.
- **Новые занятия на двоих.** Вдвоём теперь проходятся стадионы (Kickstart, Blood Ring, гонки на аренах), карьер, смена парковщика, тир Ammu-Nation (второй стреляет из соседней кабинки), кольца на BMX и NRG и курьер Биг Смоука, за мешки которого деньги получают оба.
- **Окно игроков F2.** В нём видны деньги каждого игрока, и отсюда же можно перевести деньги другому. `/tp` и `/tphere` теперь просьбы: второй игрок отвечает Y или N. При создании игры хост может запретить напарникам эти команды.
- **Стрелка на радаре.** Другой игрок виден на радаре стрелкой, которая показывает, куда он смотрит. Цвет своей метки у второго игрока каждый выбирает сам в F9 или командой `/blip`.
- **Маршрут на радаре.** В машине на радаре рисуется путь по дорогам до метки на карте (красный) или до цели миссии (жёлтый), в том числе у пассажира. Выключается настройкой `gps = 0`.
- **Оружие пассажира.** Высунувшись из окна, пассажир стреляет из любого огнестрельного оружия, а не только из пистолетов-пулемётов, и меняет его колесом мыши или крестовиной геймпада.
- **Попутчики.** Бойцы вашей банды садятся с вами, когда вы едете пассажиром в машине второго игрока.
- **Значок микрофона.** Кто сейчас говорит, видно по красному значку микрофона над головой и в списке игроков.
- **Названия сохранений.** В списке слотов в «Создать игру» теперь видны названия сохранений.
- **Стрельба из машин из другого мода.** Если у игрока стоит мод на стрельбу из машины, второй игрок видит эти выстрелы (#76).

## Исправления

- Напарник больше не вылетает в начале катсцены (#72).
- Игра больше не зависает на чёрном экране при загрузке катсцены или мира (#86, #87).
- Геймпад через GInput больше не отключается после сворачивания игры (#74).
- Если голосовой чат не может открыть звуковую карту, игра больше не вылетает при входе: голос в этот раз просто выключается (#80).
- С модами, которые расширяют лимиты игры, у обоих снова видны машины, люди и предметы другого игрока (#82).
- `install.bat` больше не ломает загрузчик ASI из Widescreen Fix (ошибка «This library isn't supported»).
- Люди хоста у напарника не только целятся, но и стреляют. Выстрелы игроков пешком видны у обоих, у копии видны трассеры, а очереди полицейского вертолёта и бортовых пулемётов видит и напарник. Громкоговоритель вертолёта он тоже слышит.
- Ближний бой идёт у обоих одинаково: удары и реакции на них совпадают.
- Голова, отрубленная катаной, возвращается после возрождения.
- Машины и люди миссий хоста больше не пропадают у напарника на минуту и дольше. Так пропадали Вагос в «Running Dog» и фургон Майка Торено.
- В миссиях с турелью («The Da Nang Thang», «Reuniting the Families») напарник больше не получает прицел и пулемёт хоста.
- Шары на бильярдном столе другого игрока теперь видны: раньше они лежали внутри стола.
- Машины в гараже у дома больше не теряются и не двоятся, и напарник забирает из гаража только свои. Ворота гаража открываются и для второго игрока.
- В поезд теперь может сесть и напарник.
- Ворота аэропорта Сан-Фиерро в миссиях Джиззи открыты у обоих.
- Метка точки сдачи груза в грузоперевозках больше не остаётся на карте навсегда.
- Замедление на уникальных прыжках больше не сбивается.
- Посадку второго игрока в машину другой видит такой, какой она была, в том числе угон катящейся машины.
- Машины и прохожие после столкновения с копией больше не становятся «каменными».
- Полицейский вертолёт над хостом виден напарнику.
- В «Sweet's Girl» напарник не пропадает из машины, а в «Tagging Up Turf» выходит из неё вместе с хостом.
- Дверь примерочной в Binco, соперник на ринге в спортзале и двери спортзалов у напарника работают так же, как у хоста.
- Второй CJ в катсценах стоит в кадре на одном месте и не прыгает между планами.
- Звонки по телефону между миссиями слышат оба, и телефон больше не застревает в руке.
- Двойной кассир в пиццерии Райдера и двойные машины на штрафстоянке пропали.
- Фонари и светофоры больше не ломаются сами по себе.
- Маркер дома при ограблении больше не пропадает, когда в дом вошёл другой игрок.
- Коктейль Молотова больше не взрывается в руке у копии.
- Музыка пройденной миссии играет и у напарника.
- Пока открыт чат, персонаж стоит на месте: Shift, Alt и пробел в сообщении больше не заставляют его прыгать.
- F1 (повтор последних секунд) в совместной игре отключён: раньше он ронял игру.
- При посадке в машину пистолет-пулемёт снова оказывается в руке, как в обычной игре, а винтовку пассажира видно, когда он высовывается из окна (#83).

## Установка

Распакуйте `SA-COOP.zip` в папку игры, чтобы `coop.asi`, `install.bat` и папка `coop` оказались рядом с `gta_sa.exe`, и запустите `install.bat`. Подробная инструкция лежит в архиве в файле `Инструкция.txt`, а описание мода в README репозитория.

Чтобы обновиться с прошлой версии, распакуйте архив поверх старых файлов. Настройки и прогресс напарника останутся, а `install.bat` запускать второй раз не нужно.

Версии 0.3.1 и 0.3.2 друг к другу не подключаются, поэтому обновиться нужно и хосту, и напарнику.

## In English

A mod for GTA San Andreas 1.0 US that lets two players go through the story together over the network.

### What's new

- **Pool for two.** Press U on the bet screen in a bar, and the other player comes over to play at your table. You take turns: the second player watches the table through your camera, shoots with their own buttons and holds the cue, and the game itself names the winner.
- **Contests.** Basketball, pool, blackjack, roulette, the wheel of fortune, club dancing, the lowrider meet, stadium events, races and the triathlon can be played against each other: press U in the first 20 seconds. Each player plays in their own game, and a board at the top of the screen shows both scores, the countdown to the start and the result in big letters.
- **Odd jobs truly for two.** In the taxi each player drives their own fares. In the ambulance, fire truck and vigilante jobs the partner gets their own vehicle and works on a par with the host. In burglary both carry loot into one van, and in the pimping job either player can pick up the girl.
- **New activities for two.** Stadium events (Kickstart, Blood Ring, arena races), the quarry, the valet shift, the Ammu-Nation shooting range (the second player shoots from the next booth), the BMX and NRG rings and Big Smoke's courier can now be done together, and both get the money for the courier's bags.
- **Players window, F2.** It shows each player's money, and you can send money to the other player from it. `/tp` and `/tphere` are requests now: the other player answers Y or N. When creating a game, the host can forbid these commands for partners.
- **Radar arrow.** The other player shows on the radar as an arrow pointing where they look. Each player picks the colour of their own mark for the other player in F9 or with `/blip`.
- **Route on the radar.** In a vehicle the radar shows the road route to the map waypoint (red) or to the mission target (yellow), for passengers too. `gps = 0` turns it off.
- **Passenger weapons.** A passenger leaning out of the window can fire any firearm, not only submachine guns, and switches weapons with the mouse wheel or the gamepad's d-pad.
- **Followers.** Your gang members get in with you when you ride as a passenger in the other player's car.
- **Microphone icon.** A red microphone icon above the head and in the players list shows who is talking.
- **Save names.** The slot list in "Create game" now shows the saves' names.
- **Shooting from cars with another mod.** If a player has a mod for shooting from vehicles, the other player sees those shots (#76).

### Fixes

- The partner no longer crashes at the start of a cutscene (#72).
- The game no longer hangs on a black screen while loading a cutscene or the world (#86, #87).
- A GInput gamepad no longer stops working after the game is minimised (#74).
- If the voice chat cannot open the sound card, the game no longer crashes on joining: the voice is just off this time (#80).
- With mods that raise the game's limits, both players see the other's cars, people and objects again (#82).
- `install.bat` no longer breaks the ASI loader of Widescreen Fix (the "This library isn't supported" error).
- The host's people no longer just aim on the partner's screen, they shoot too. Players' shots on foot are seen by both, the copy's shots leave tracers, and the partner sees the bursts of the police helicopter and of mounted guns. The partner hears the helicopter's loudspeaker too.
- Melee looks the same for both: the blows and the reactions to them match.
- A head cut off with a katana comes back after respawning.
- The cars and people of the host's missions no longer vanish for the partner for a minute or longer. That is how the Vagos of Running Dog and Mike Toreno's van went missing.
- In turret missions (The Da Nang Thang, Reuniting the Families) the partner no longer gets the host's crosshair and gun.
- The balls on the other player's pool table are visible now: before, they lay inside the table.
- Cars in a safe house garage are no longer lost or doubled, and the partner takes only their own cars out of it. The garage door opens for the other player too.
- The partner can now get on a train.
- The San Fierro airport gates in Jizzy's missions are open for both.
- The drop-off marker of the trucking job no longer stays on the map for good.
- The slow motion of unique stunt jumps is no longer cut short.
- The other player getting into a car looks the way it was, including jacking a rolling car.
- Cars and pedestrians no longer turn into "stone" after hitting the other player's copy.
- The police helicopter above the host is visible to the partner.
- In Sweet's Girl the partner no longer vanishes from the car, and in Tagging Up Turf the partner gets out together with the host.
- The Binco changing room door, the gym sparring opponent and the gym doors work for the partner as for the host.
- The second CJ in cutscenes stays in one place in the picture and no longer jumps between shots.
- Both players hear the phone calls between missions, and the phone no longer gets stuck in the hand.
- The doubled cashier in Ryder's pizzeria and the doubled cars at the impound lot are gone.
- Street lamps and traffic lights no longer break by themselves.
- In burglary the house marker no longer disappears once the other player goes in.
- A Molotov cocktail no longer explodes in the copy's hand.
- The passed-mission jingle plays for the partner too.
- While the chat is open, the character stands still: Shift, Alt and the space in a message no longer make them jump.
- F1 (replay of the last seconds) is off in a co-op game: it used to crash the game.
- When getting into a car, the submachine gun is in hand again as in the normal game, and a passenger's rifle is visible when they lean out (#83).

### Installation

Unpack `SA-COOP.zip` into the game folder so that `coop.asi`, `install.bat` and the `coop` folder sit next to `gta_sa.exe`, and run `install.bat`. The archive has detailed instructions in `Инструкция.txt` (Russian), and the repository's README describes the mod.

To update from the previous version, unpack the archive over the old files. The settings and the partner's progress stay, and `install.bat` does not need to run again.

Versions 0.3.1 and 0.3.2 cannot connect to each other, so both the host and the partner need to update.
