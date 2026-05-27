/*
** cheatmenu.zs
** Game specific cheat launcher menu with submenu pages.
*/

class CheatMenu : GenericMenu
{
	const HEADER_Y = 10;
	const LIST_LEFT = 18;
	const LIST_TOP = 40;
	const LIST_RIGHT_PAD = 18;
	const FOOTER_GAP = 10;

	Array<String> mEntryText;
	Array<String> mEntryCommand;
	Array<Name> mEntryTarget;
	Array<bool> mEntryNeedsCheats;
	Array<bool> mEntrySelectable;
	Array<bool> mEntryIsSubmenu;
	Font mFont;
	Name mPage;
	bool mRootPage;
	int mSelected;
	int mTop;

	private native static void DoCommand(String cmd, bool is_unsafe);

	virtual void SetupPage()
	{
		mPage = 'None';
		mRootPage = true;
	}

	override void Init(Menu parent)
	{
		Super.Init(parent);
		DontDim = false;
		DontBlur = false;
		Animated = false;
		AnimatedTransition = false;
		mFont = Menu.OptionFont();
		mSelected = 0;
		mTop = 0;
		mEntryText.Clear();
		mEntryCommand.Clear();
		mEntryTarget.Clear();
		mEntryNeedsCheats.Clear();
		mEntrySelectable.Clear();
		mEntryIsSubmenu.Clear();
		SetupPage();
		BuildMenu();
		EnsureVisible();
	}

	override bool TranslateKeyboardEvents()
	{
		return true;
	}

	private void AddCommand(String text, String command, bool needsCheats = false)
	{
		mEntryText.Push(text);
		mEntryCommand.Push(command);
		mEntryTarget.Push('None');
		mEntryNeedsCheats.Push(needsCheats);
		mEntrySelectable.Push(true);
		mEntryIsSubmenu.Push(false);
	}

	private void AddSubmenu(String text, Name target)
	{
		mEntryText.Push(text);
		mEntryCommand.Push("");
		mEntryTarget.Push(target);
		mEntryNeedsCheats.Push(false);
		mEntrySelectable.Push(true);
		mEntryIsSubmenu.Push(true);
	}

	private void AddInfo(String text)
	{
		mEntryText.Push(text);
		mEntryCommand.Push("");
		mEntryTarget.Push('None');
		mEntryNeedsCheats.Push(false);
		mEntrySelectable.Push(false);
		mEntryIsSubmenu.Push(false);
	}

	private void CloseMenuStack()
	{
		Close();
		let cur = Menu.GetCurrentMenu();
		if (cur != null)
		{
			cur.Close();
		}
	}

	private int RowHeight()
	{
		return mFont.GetHeight() * CleanYfac_1 + 4;
	}

	private bool IsSelectable(int index)
	{
		return index >= 0 && index < mEntryText.Size() && mEntrySelectable[index];
	}

	private int FirstSelectable()
	{
		for (int i = 0; i < mEntryText.Size(); ++i)
		{
			if (IsSelectable(i)) return i;
		}
		return -1;
	}

	private void NormalizeSelection()
	{
		int first = FirstSelectable();
		if (first < 0)
		{
			mSelected = -1;
			mTop = 0;
			return;
		}
		if (!IsSelectable(mSelected))
		{
			mSelected = first;
		}
	}

	private int VisibleRows()
	{
		int rowHeight = RowHeight();
		int usable = screen.GetHeight() - LIST_TOP - FOOTER_GAP - (mFont.GetHeight() * CleanYfac_1 * 3);
		int rows = usable / rowHeight;
		if (rows < 5) rows = 5;
		if (rows > mEntryText.Size()) rows = mEntryText.Size();
		return rows;
	}

	private void EnsureVisible()
	{
		NormalizeSelection();
		if (mSelected < 0) return;
		int rows = VisibleRows();
		if (mSelected < mTop) mTop = mSelected;
		else if (mSelected >= mTop + rows) mTop = mSelected - rows + 1;
		if (mTop < 0) mTop = 0;
		if (mTop > mEntryText.Size() - rows) mTop = max(0, mEntryText.Size() - rows);
	}

	private void MoveSelection(int delta)
	{
		if (mEntryText.Size() == 0) return;
		int idx = mSelected;
		if (idx < 0) idx = FirstSelectable();
		if (idx < 0) return;

		for (int i = 0; i < mEntryText.Size(); ++i)
		{
			idx += delta;
			if (idx < 0) idx = mEntryText.Size() - 1;
			else if (idx >= mEntryText.Size()) idx = 0;

			if (IsSelectable(idx))
			{
				mSelected = idx;
				EnsureVisible();
				return;
			}
		}
	}

	private String GameTitle()
	{
		if (gameinfo.gametype == GAME_Doom) return "DOOM / ULTIMATE DOOM / SIGIL / SIGIL II";
		if (gameinfo.gametype == GAME_Chex) return "CHEX QUEST 3";
		if (gameinfo.gametype == GAME_Heretic) return "HERETIC: SHADOW OF THE SERPENT RIDERS";
		if (gameinfo.gametype == GAME_Hexen) return "HEXEN: BEYOND HERETIC";
		if (gameinfo.gametype == GAME_Strife) return "STRIFE: QUEST FOR THE SIGIL";
		return "UNKNOWN GAME";
	}

	private String PageTitle()
	{
		if (mRootPage) return "Main";
		if (mPage == 'doom_items' || mPage == 'chex_items' || mPage == 'heretic_items' || mPage == 'hexen_items' || mPage == 'strife_items') return "Item/weapon spawner";
		if (mPage == 'doom_monsters' || mPage == 'chex_monsters' || mPage == 'heretic_monsters' || mPage == 'hexen_monsters' || mPage == 'strife_monsters') return "Monster spawner";
		if (mPage == 'doom_levels' || mPage == 'chex_levels' || mPage == 'heretic_levels' || mPage == 'hexen_levels' || mPage == 'strife_levels') return "Level select";
		return "Core cheats";
	}

	private void ActivateSelected()
	{
		if (!IsSelectable(mSelected))
		{
			return;
		}

		if (mEntryIsSubmenu[mSelected])
		{
			Menu.MenuSound("menu/advance");
			Menu.SetMenu(mEntryTarget[mSelected]);
			return;
		}

		if (mEntryCommand[mSelected].Length() == 0)
		{
			return;
		}

		Menu.MenuSound("menu/choose");
		Close();
		if (mEntryNeedsCheats[mSelected])
		{
			DoCommand("sv_cheats 1; " .. mEntryCommand[mSelected], false);
		}
		else
		{
			DoCommand(mEntryCommand[mSelected], false);
		}
	}

	override bool MenuEvent(int key, bool fromcontroller)
	{
		if (key == MKEY_Back)
		{
			CloseMenuStack();
			return true;
		}

		switch (key)
		{
		case MKEY_Enter:
		case MKEY_Input:
			ActivateSelected();
			return true;

		case MKEY_Up:
			MoveSelection(-1);
			Menu.MenuSound("menu/cursor");
			return true;

		case MKEY_Down:
			MoveSelection(1);
			Menu.MenuSound("menu/cursor");
			return true;

		case MKEY_PageUp:
			for (int i = 0; i < max(1, VisibleRows() - 1); ++i) MoveSelection(-1);
			Menu.MenuSound("menu/cursor");
			return true;

		case MKEY_PageDown:
			for (int i = 0; i < max(1, VisibleRows() - 1); ++i) MoveSelection(1);
			Menu.MenuSound("menu/cursor");
			return true;
		}

		return false;
	}

	override bool OnUIEvent(UIEvent ev)
	{
		if (ev.Type == UIEvent.Type_WheelUp)
		{
			MoveSelection(-1);
			Menu.MenuSound("menu/cursor");
			return true;
		}
		else if (ev.Type == UIEvent.Type_WheelDown)
		{
			MoveSelection(1);
			Menu.MenuSound("menu/cursor");
			return true;
		}
		else if (ev.Type == UIEvent.Type_KeyDown)
		{
			if (ev.KeyChar == UIEvent.Key_Escape || ev.KeyChar == UIEvent.Key_Back)
			{
				CloseMenuStack();
				return true;
			}
			if (ev.KeyChar == 13)
			{
				ActivateSelected();
				return true;
			}
		}

		return Super.OnUIEvent(ev);
	}

	override bool OnInputEvent(InputEvent ev)
	{
		if (ev.type == InputEvent.Type_KeyDown)
		{
			int toggleKey1, toggleKey2;
			int consoleKey1, consoleKey2;
			[toggleKey1, toggleKey2] = Bindings.GetKeysForCommand("togglecheatmenu");
			[consoleKey1, consoleKey2] = Bindings.GetKeysForCommand("toggleconsole");
			if (ev.KeyScan == toggleKey1 || ev.KeyScan == toggleKey2 ||
				ev.KeyScan == consoleKey1 || ev.KeyScan == consoleKey2 ||
				ev.KeyScan == InputEvent.Key_Pad_B || ev.KeyScan == InputEvent.Key_Escape)
			{
				CloseMenuStack();
				return true;
			}
		}
		return Super.OnInputEvent(ev);
	}

	override bool MouseEvent(int type, int x, int y)
	{
		int rowHeight = RowHeight();
		int listTop = LIST_TOP;
		int listBottom = listTop + VisibleRows() * rowHeight;
		int listWidth = screen.GetWidth() - LIST_LEFT * 2 - LIST_RIGHT_PAD;

		if (x < LIST_LEFT || x >= LIST_LEFT + listWidth || y < listTop || y >= listBottom)
		{
			return Super.MouseEvent(type, x, y);
		}

		int index = mTop + (y - listTop) / rowHeight;
		if (IsSelectable(index))
		{
			mSelected = index;
			EnsureVisible();
			if (type == MOUSE_Release)
			{
				ActivateSelected();
			}
			return true;
		}

		return Super.MouseEvent(type, x, y);
	}

	private void BuildMenu()
	{
		if (mRootPage)
		{
			BuildRootPage();
			return;
		}

		if (mPage == 'doom_items') BuildDoomItems();
		else if (mPage == 'doom_monsters') BuildDoomMonsters();
		else if (mPage == 'doom_levels') BuildDoomLevels();
		else if (mPage == 'chex_items') BuildChexItems();
		else if (mPage == 'chex_monsters') BuildChexMonsters();
		else if (mPage == 'chex_levels') BuildChexLevels();
		else if (mPage == 'heretic_items') BuildHereticItems();
		else if (mPage == 'heretic_monsters') BuildHereticMonsters();
		else if (mPage == 'heretic_levels') BuildHereticLevels();
		else if (mPage == 'hexen_items') BuildHexenItems();
		else if (mPage == 'hexen_monsters') BuildHexenMonsters();
		else if (mPage == 'hexen_levels') BuildHexenLevels();
		else if (mPage == 'strife_items') BuildStrifeItems();
		else if (mPage == 'strife_monsters') BuildStrifeMonsters();
		else if (mPage == 'strife_levels') BuildStrifeLevels();
		else AddInfo("NO COMPATIBLE IWAD FOUND");
	}

	private void BuildRootPage()
	{
		AddCommand("God mode ON/OFF", "iddqd", true);
		AddCommand("Walk through walls ON/OFF", "noclip", true);
		AddCommand("Invisible to Enemies ON/OFF", "notarget", true);
		AddCommand("Monsters fear you ON/OFF", "anubis", true);
		AddCommand("Freeze everything ON/OFF", "freeze", true);
		AddCommand("All keys, weapons, armor", "give all", true);
		AddCommand("Kill all monsters", "kill monsters", true);
		AddCommand("Resurrect dead player", "resurrect", true);
		AddCommand("Go to the next map", "nextmap");
		if (gameinfo.gametype == GAME_Doom)
		{
			AddSubmenu("Item/weapon spawner", 'CheatMenuDoomItems');
			AddSubmenu("Monster spawner", 'CheatMenuDoomMonsters');
			AddSubmenu("Level select", 'CheatMenuDoomLevels');
		}
		else if (gameinfo.gametype == GAME_Chex)
		{
			AddSubmenu("Item/weapon spawner", 'CheatMenuChexItems');
			AddSubmenu("Monster spawner", 'CheatMenuChexMonsters');
			AddSubmenu("Level select", 'CheatMenuChexLevels');
		}
		else if (gameinfo.gametype == GAME_Heretic)
		{
			AddSubmenu("Item/weapon spawner", 'CheatMenuHereticItems');
			AddSubmenu("Monster spawner", 'CheatMenuHereticMonsters');
			AddSubmenu("Level select", 'CheatMenuHereticLevels');
		}
		else if (gameinfo.gametype == GAME_Hexen)
		{
			AddSubmenu("Item/weapon spawner", 'CheatMenuHexenItems');
			AddSubmenu("Monster spawner", 'CheatMenuHexenMonsters');
			AddSubmenu("Level select", 'CheatMenuHexenLevels');
		}
		else if (gameinfo.gametype == GAME_Strife)
		{
			AddSubmenu("Item/weapon spawner", 'CheatMenuStrifeItems');
			AddSubmenu("Monster spawner", 'CheatMenuStrifeMonsters');
			AddSubmenu("Level select", 'CheatMenuStrifeLevels');
		}
		else
		{
			AddInfo("NO COMPATIBLE IWAD FOUND");
		}
		AddCommand("Developer Mode", "developer 1");
		AddCommand("Debug Mode", "developer 2");
		AddCommand("Remove Developer Mode", "developer 0");
	}

	private void BuildDoomItems()
	{
		AddCommand("Pistol", "summon Pistol", true);
		AddCommand("Chainsaw", "summon Chainsaw", true);
		AddCommand("Shotgun", "summon Shotgun", true);
		AddCommand("Chaingun", "summon Chaingun", true);
		AddCommand("Rocket Launcher", "summon Rocketlauncher", true);
		AddCommand("Plasma Rifle", "summon PlasmaRifle", true);
		AddCommand("BFG 9000", "summon BFG9000", true);
		AddCommand("Backpack", "summon Backpack", true);
		AddCommand("Ammo Clip", "summon Clip", true);
		AddCommand("Box of Bullets", "summon ClipBox", true);
		AddCommand("4 Shells", "summon Shell", true);
		AddCommand("Box of Shells", "summon ShellBox", true);
		AddCommand("Rocket", "summon RocketAmmo", true);
		AddCommand("Box of Rockets", "summon RocketBox", true);
		AddCommand("Cell", "summon Cell", true);
		AddCommand("Cell Pack", "summon CellPack", true);
		AddCommand("Blue Keycard", "summon BlueCard", true);
		AddCommand("Red Keycard", "summon RedCard", true);
		AddCommand("Yellow Keycard", "summon YellowCard", true);
		AddCommand("Blue Skull Key", "summon BlueSkull", true);
		AddCommand("Red Skull Key", "summon RedSkull", true);
		AddCommand("Yellow Skull Key", "summon YellowSkull", true);
		AddCommand("Computer Area Map", "summon Allmap", true);
		AddCommand("Megasphere (+200 Health/Armor)", "summon Megasphere", true);
		AddCommand("Soul Sphere (+100 Health)", "summon SoulSphere", true);
		AddCommand("Medikit(+25 Health)", "summon Medikit", true);
		AddCommand("Stimpack(+10 Health)", "summon Stimpack", true);
		AddCommand("Health Bonus", "summon HealthBonus", true);
		AddCommand("Heavy Armor", "summon BlueArmor", true);
		AddCommand("Light Armor", "summon GreenArmor", true);
		AddCommand("Armor Helmet", "summon ArmorBonus", true);
		AddCommand("Berserk Pack", "summon Berserk", true);
		AddCommand("Partial Invisibility", "summon BlurSphere", true);
		AddCommand("Light-Amp Goggles", "summon Infrared", true);
		AddCommand("Radiation Suit", "summon RadSuit", true);
		AddCommand("Invulnerability", "summon InvulnerabilitySphere", true);
	}

	private void BuildDoomMonsters()
	{
		AddCommand("Zombieman", "summon zombieman", true);
		AddCommand("Former Sergeant", "summon shotgunguy", true);
		AddCommand("Chaingunner", "summon chaingunguy", true);
		AddCommand("Imp", "summon doomimp", true);
		AddCommand("Pinky", "summon demon", true);
		AddCommand("Spectre", "summon spectre", true);
		AddCommand("Cacodemon", "summon Cacodemon", true);
		AddCommand("Lost Soul", "summon Lostsoul", true);
		AddCommand("Mancubus", "summon Fatso", true);
		AddCommand("Revenant", "summon Revenant", true);
		AddCommand("Baron of Hell", "summon BaronOfHell", true);
		AddCommand("Hell Knight", "summon hellknight", true);
		AddCommand("Pain Elemental", "summon PainElemental", true);
		AddCommand("Arachnotron", "summon Arachnotron", true);
		AddCommand("Spider Mastermind", "summon spidermastermind", true);
		AddCommand("Cyberdemon", "summon Cyberdemon", true);
	}

	private void BuildDoomLevels()
	{
		AddInfo("DOOM / ULTIMATE DOOM / SIGIL / SIGIL II");
		AddCommand("Hangar - E1M1", "idclev 11", true);
		AddCommand("Nuclear Plant - E1M2", "idclev 12", true);
		AddCommand("Toxin Refinery - E1M3", "idclev 13", true);
		AddCommand("Command Control - E1M4", "idclev 14", true);
		AddCommand("Phobos Lab - E1M5", "idclev 15", true);
		AddCommand("Central Processing - E1M6", "idclev 16", true);
		AddCommand("Computer Station - E1M7", "idclev 17", true);
		AddCommand("Phobos Anomaly - E1M8", "idclev 18", true);
		AddCommand("Military Base - E1M9", "idclev 19", true);
		AddInfo("");
		AddCommand("Deimos Anomaly - E2M1", "idclev 21", true);
		AddCommand("Containment Area - E2M2", "idclev 22", true);
		AddCommand("Refinery - E2M3", "idclev 23", true);
		AddCommand("Deimos Lab - E2M4", "idclev 24", true);
		AddCommand("Command Center - E2M5", "idclev 25", true);
		AddCommand("Halls of the Damned - E2M6", "idclev 26", true);
		AddCommand("Spawning Vats - E2M7", "idclev 27", true);
		AddCommand("Tower of Babel - E2M8", "idclev 28", true);
		AddCommand("Fortress of Mystery - E2M9", "idclev 29", true);
		AddInfo("");
		AddCommand("Hell Keep - E3M1", "idclev 31", true);
		AddCommand("Slough of Despair - E3M2", "idclev 32", true);
		AddCommand("Pandemonium - E3M3", "idclev 33", true);
		AddCommand("House of Pain - E3M4", "idclev 34", true);
		AddCommand("Unholy Cathedral - E3M5", "idclev 35", true);
		AddCommand("Mt. Erebus - E3M6", "idclev 36", true);
		AddCommand("Gate to Limbo - E3M7", "idclev 37", true);
		AddCommand("Dis - E3M8", "idclev 38", true);
		AddCommand("Warrens - E3M9", "idclev 39", true);
		AddInfo("");
		AddInfo("ULTIMATE DOOM ONLY");
		AddCommand("Hell Beneath - E4M1", "idclev 41", true);
		AddCommand("Perfect Hatred - E4M2", "idclev 42", true);
		AddCommand("Sever the Wicked - E4M3", "idclev 43", true);
		AddCommand("Unruly Evil - E4M4", "idclev 44", true);
		AddCommand("They will Repent - E4M5", "idclev 45", true);
		AddCommand("Against Thee Wickedly - E4M6", "idclev 46", true);
		AddCommand("And Hell Followed - E4M7", "idclev 47", true);
		AddCommand("Unto the Cruel - E4M8", "idclev 48", true);
		AddCommand("Fear - E4M9", "idclev 49", true);
		AddInfo("");
		AddInfo("SIGIL ONLY");
		AddCommand("Baphomet's Demense - E5M1", "idclev 51", true);
		AddCommand("Sheol - E5M2", "idclev 52", true);
		AddCommand("Cages of the Damned - E5M3", "idclev 53", true);
		AddCommand("Path of Wretchedness - E5M4", "idclev 54", true);
		AddCommand("Abbadon's Void - E5M5", "idclev 55", true);
		AddCommand("Unspeakable Persecution - E5M6", "idclev 56", true);
		AddCommand("Nichtmare Underworld - E5M7", "idclev 57", true);
		AddCommand("Halls of Perdition - E5M8", "idclev 58", true);
		AddCommand("Realm of Iblis - E5M9", "idclev 59", true);
		AddInfo("");
		AddInfo("SIGIL II ONLY");
		AddCommand("Cursed Darkness - E6M1", "idclev 61", true);
		AddCommand("Violent Hatred - E6M2", "idclev 62", true);
		AddCommand("Twilight Desolution - E6M3", "idclev 63", true);
		AddCommand("Fragments of Sanity - E6M4", "idclev 64", true);
		AddCommand("Wrathful Reckening - E6M5", "idclev 65", true);
		AddCommand("Vengeance Unleashed - E6M6", "idclev 66", true);
		AddCommand("Decent into Terror - E6M7", "idclev 67", true);
		AddCommand("Abyss of Despair - E6M8", "idclev 68", true);
		AddCommand("Shattered Homecoming - E6M9", "idclev 69", true);
		AddInfo("");
		AddInfo("DOOM II: HELL ON EARTH");
		AddCommand("Entryway - Map 01", "idclev 01", true);
		AddCommand("Underhalls - Map 02", "idclev 02", true);
		AddCommand("The Gantlet - Map 03", "idclev 03", true);
		AddCommand("The Focus - Map 04", "idclev 04", true);
		AddCommand("The Waste Tunnels - Map 05", "idclev 05", true);
		AddCommand("The Crusher - Map 06", "idclev 06", true);
		AddCommand("Dead Simple - Map 07", "idclev 07", true);
		AddCommand("Tricks and Traps - Map 08", "idclev 08", true);
		AddCommand("The Pit - Map 09", "idclev 09", true);
		AddCommand("Refueling Base - Map 10", "idclev 10", true);
		AddCommand("Circle of Death - Map 11", "idclev 11", true);
		AddCommand("The Factory - Map 12", "idclev 12", true);
		AddCommand("Downtown - Map 13", "idclev 13", true);
		AddCommand("The Inmost Dense - Map 14", "idclev 14", true);
		AddCommand("Industrial Zone - Map 15", "idclev 15", true);
		AddCommand("Suburbs - Map 16", "idclev 16", true);
		AddCommand("Tenements - Map 17", "idclev 17", true);
		AddCommand("The Courtyard - Map 18", "idclev 18", true);
		AddCommand("The Citadel - Map 19", "idclev 19", true);
		AddCommand("Gotcha! - Map 20", "idclev 20", true);
		AddCommand("Nirvana - Map 21", "idclev 21", true);
		AddCommand("The Catacombs - Map 22", "idclev 22", true);
		AddCommand("Barrels O' Fun - Map 23", "idclev 23", true);
		AddCommand("The Chasm - Map 24", "idclev 24", true);
		AddCommand("Bloodfalls - Map 25", "idclev 25", true);
		AddCommand("The Abandoned Mines - Map 26", "idclev 26", true);
		AddCommand("Monster Condo - Map 27", "idclev 27", true);
		AddCommand("The Spirit World - Map 28", "idclev 28", true);
		AddCommand("The Living End - Map 29", "idclev 29", true);
		AddCommand("Icon of Sin - Map 30", "idclev 30", true);
		AddCommand("Wolfenstein - Map 31", "idclev 31", true);
		AddCommand("Grosse - Map 32", "idclev 32", true);
		AddInfo("");
		AddInfo("FINAL DOOM: TNT EVILUTION");
		AddCommand("System Control - Map 01", "idclev 01", true);
		AddCommand("Human BBQ - Map 02", "idclev 02", true);
		AddCommand("Power Control - Map 03", "idclev 03", true);
		AddCommand("Wormhole - Map 04", "idclev 04", true);
		AddCommand("Hanger - Map 05", "idclev 05", true);
		AddCommand("Open Season - Map 06", "idclev 06", true);
		AddCommand("Prison - Map 07", "idclev 07", true);
		AddCommand("Metal - Map 08", "idclev 08", true);
		AddCommand("Stronghold - Map 09", "idclev 09", true);
		AddCommand("Redemption - Map 10", "idclev 10", true);
		AddCommand("Storage Facility - Map 11", "idclev 11", true);
		AddCommand("Crater - Map 12", "idclev 12", true);
		AddCommand("Nukage Processing - Map 13", "idclev 13", true);
		AddCommand("Steel Works - Map 14", "idclev 14", true);
		AddCommand("Dead Zone - Map 15", "idclev 15", true);
		AddCommand("Deepest Reaches - Map 16", "idclev 16", true);
		AddCommand("Processing Area - Map 17", "idclev 17", true);
		AddCommand("Mill - Map 18", "idclev 18", true);
		AddCommand("Shipping/Respawning - Map 19", "idclev 19", true);
		AddCommand("Central Processing - Map 20", "idclev 20", true);
		AddCommand("Administration Center - Map 21", "idclev 21", true);
		AddCommand("Habitat - Map 22", "idclev 22", true);
		AddCommand("Lunar Mining Project - Map 23", "idclev 23", true);
		AddCommand("Quarry - Map 24", "idclev 24", true);
		AddCommand("Baron's Den - Map 25", "idclev 25", true);
		AddCommand("Ballistyx - Map 26", "idclev 26", true);
		AddCommand("Mount Pain - Map 27", "idclev 27", true);
		AddCommand("Heck - Map 28", "idclev 28", true);
		AddCommand("River Styx - Map 29", "idclev 29", true);
		AddCommand("Last Call - Map 30", "idclev 30", true);
		AddCommand("Pharaoh - Map 31", "idclev 31", true);
		AddCommand("Caribbean - Map 32", "idclev 32", true);
		AddInfo("");
		AddInfo("FINAL DOOM: PLUTONIA EXPERIMENT");
		AddCommand("Congo - Map 01", "idclev 01", true);
		AddCommand("Well of Souls - Map 02", "idclev 02", true);
		AddCommand("Aztec - Map 03", "idclev 03", true);
		AddCommand("Caged - Map 04", "idclev 04", true);
		AddCommand("Ghost Town - Map 05", "idclev 05", true);
		AddCommand("Baron's Lair - Map 06", "idclev 06", true);
		AddCommand("Caughtyard - Map 07", "idclev 07", true);
		AddCommand("Realm - Map 08", "idclev 08", true);
		AddCommand("Abattoire - Map 09", "idclev 09", true);
		AddCommand("Onslaught - Map 10", "idclev 10", true);
		AddCommand("Hunted - Map 11", "idclev 11", true);
		AddCommand("Speed - Map 12", "idclev 12", true);
		AddCommand("The Crypt - Map 13", "idclev 13", true);
		AddCommand("Genesis - Map 14", "idclev 14", true);
		AddCommand("The Twilight - Map 15", "idclev 15", true);
		AddCommand("The Omen - Map 16", "idclev 16", true);
		AddCommand("Compund - Map 17", "idclev 17", true);
		AddCommand("Neurosphere - Map 18", "idclev 18", true);
		AddCommand("NME - Map 19", "idclev 19", true);
		AddCommand("The Death Domain - Map 20", "idclev 20", true);
		AddCommand("Slayer - Map 21", "idclev 21", true);
		AddCommand("Impossible Mission - Map 22", "idclev 22", true);
		AddCommand("Tombstone - Map 23", "idclev 23", true);
		AddCommand("The Final Frontier - Map 24", "idclev 24", true);
		AddCommand("The Temple of Darkness - Map 25", "idclev 25", true);
		AddCommand("Bunker - Map 26", "idclev 26", true);
		AddCommand("Anti-Christ - Map 27", "idclev 27", true);
		AddCommand("The Sewers - Map 28", "idclev 28", true);
		AddCommand("Odyssey of Noises - Map 29", "idclev 29", true);
		AddCommand("The Gateway of Hell - Map 30", "idclev 30", true);
		AddCommand("Cyberden - Map 31", "idclev 31", true);
		AddCommand("Go 2 It - Map 32", "idclev 32", true);
		AddInfo("");
		AddInfo("FREEDOOM: PHASE 1");
		AddCommand("Outer Prison - C1M1", "idclev 11", true);
		AddCommand("Communications Center - C1M2", "idclev 12", true);
		AddCommand("Waste Disposal - C1M3", "idclev 13", true);
		AddCommand("Supply Depot - C1M4", "idclev 14", true);
		AddCommand("Main Control - C1M5", "idclev 15", true);
		AddCommand("Training Facility - C1M6", "idclev 16", true);
		AddCommand("Transportation Bay - C1M7", "idclev 17", true);
		AddCommand("Outpost Quarry - C1M8", "idclev 18", true);
		AddCommand("Armory - C1M9", "idclev 19", true);
		AddInfo("");
		AddCommand("Ruins - C2M1", "idclev 21", true);
		AddCommand("Power Plant - C2M2", "idclev 22", true);
		AddCommand("Archaelogy Site - C2M3", "idclev 23", true);
		AddCommand("Sample Holding Site - C2M4", "idclev 24", true);
		AddCommand("Fortress 31 - C2M5", "idclev 25", true);
		AddCommand("Trepidation Site - C2M6", "idclev 26", true);
		AddCommand("Quarantine Vessel - C2M7", "idclev 27", true);
		AddCommand("Containment Cell - C2M8", "idclev 28", true);
		AddCommand("Corruption of Man - C2M9", "idclev 29", true);
		AddInfo("");
		AddCommand("Land of the Lost - C3M1", "idclev 31", true);
		AddCommand("Infernal Caverns - C3M2", "idclev 32", true);
		AddCommand("Derelict Temple - C3M3", "idclev 33", true);
		AddCommand("Sacrificial Bastion - C3M4", "idclev 34", true);
		AddCommand("Oblation Temple - C3M5", "idclev 35", true);
		AddCommand("Igneous Intrusion - C3M6", "idclev 36", true);
		AddCommand("No Regrets - C3M7", "idclev 37", true);
		AddCommand("Ancient Lair - C3M8", "idclev 38", true);
		AddCommand("Acquainted With Grief - C3M9", "idclev 39", true);
		AddInfo("");
		AddCommand("Maintenance Area - C4M1", "idclev 41", true);
		AddCommand("Research Complex - C4M2", "idclev 42", true);
		AddCommand("Central Computing - C4M3", "idclev 43", true);
		AddCommand("Hydroponic Facility - C4M4", "idclev 44", true);
		AddCommand("Engineering Station - C4M5", "idclev 45", true);
		AddCommand("Command Center - C4M6", "idclev 46", true);
		AddCommand("Waste Treatment - C4M7", "idclev 47", true);
		AddCommand("Launch Bay - C4M8", "idclev 48", true);
		AddCommand("Operations - C4M9", "idclev 49", true);
		AddInfo("");
		AddInfo("FREEDOOM: PHASE 2");
		AddCommand("Hydroelectric Plant - Map 01", "idclev 01", true);
		AddCommand("Filtration Tunnels - Map 02", "idclev 02", true);
		AddCommand("Crude Processing Center - Map 03", "idclev 03", true);
		AddCommand("Containment Bay - Map 04", "idclev 04", true);
		AddCommand("Sludge Burrow - Map 05", "idclev 05", true);
		AddCommand("Gamma Labs - Map 06", "idclev 06", true);
		AddCommand("Outer Storage Warehouse - Map 07", "idclev 07", true);
		AddCommand("Astronomy Complex - Map 08", "idclev 08", true);
		AddCommand("Datacenter - Map 09", "idclev 09", true);
		AddCommand("Deadly Outlands - Map 10", "idclev 10", true);
		AddCommand("Infinite Plain - Map 11", "idclev 11", true);
		AddCommand("Railroads - Map 12", "idclev 12", true);
		AddCommand("Station Earth - Map 13", "idclev 13", true);
		AddCommand("Nuclear Zone - Map 14", "idclev 14", true);
		AddCommand("Hostile Takeover - Map 15", "idclev 15", true);
		AddCommand("Urban Jungle - Map 16", "idclev 16", true);
		AddCommand("City Capitol - Map 17", "idclev 17", true);
		AddCommand("Aquatics Lab - Map 18", "idclev 18", true);
		AddCommand("Sewage Control - Map 19", "idclev 19", true);
		AddCommand("Blood Ember Fortress - Map 20", "idclev 20", true);
		AddCommand("Under Realm - Map 21", "idclev 21", true);
		AddCommand("Remanasu - Map 22", "idclev 22", true);
		AddCommand("Underground Facility - Map 23", "idclev 23", true);
		AddCommand("Tertiary Loading Bay - Map 24", "idclev 24", true);
		AddCommand("Red Works - Map 25", "idclev 25", true);
		AddCommand("Dark Depths - Map 26", "idclev 26", true);
		AddCommand("Warped Elementality - Map 27", "idclev 27", true);
		AddCommand("Grim Redoubt - Map 28", "idclev 28", true);
		AddCommand("Last Stand - Map 29", "idclev 29", true);
		AddCommand("Jaws of Defeat - Map 30", "idclev 30", true);
		AddCommand("Be Quiet - Map 31", "idclev 31", true);
		AddCommand("Not Sure - Map 32", "idclev 32", true);
		AddInfo("");
		AddInfo("HACX");
		AddCommand("GenEmp Corp - Map 01", "idclev 01", true);
		AddCommand("Tunnel Town - Map 02", "idclev 02", true);
		AddCommand("Lava Annex - Map 03", "idclev 03", true);
		AddCommand("Alcatraz - Map 04", "idclev 04", true);
		AddCommand("Cyber Circus - Map 05", "idclev 05", true);
		AddCommand("Digi-Ota - Map 06", "idclev 06", true);
		AddInfo("");
		AddCommand("The Great Wall - Map 07", "idclev 07", true);
		AddCommand("Garden of Delight - Map 08", "idclev 08", true);
		AddCommand("Hidden Fortress - Map 09", "idclev 09", true);
		AddCommand("Anarchist Dream - Map 10", "idclev 10", true);
		AddCommand("Notus Us! - Map 11", "idclev 11", true);
		AddInfo("");
		AddCommand("Gothik Gauntlet - Map 12", "idclev 12", true);
		AddCommand("The Sewers - Map 13", "idclev 13", true);
		AddCommand("Trode Wars - Map 14", "idclev 14", true);
		AddCommand("Twilight of EnKs - Map 15", "idclev 15", true);
		AddInfo("");
		AddCommand("Dessicant Room - Map 31", "idclev 31", true);
		AddCommand("Protean Cybex - Map 16", "idclev 16", true);
		AddCommand("River of Blood - Map 17", "idclev 17", true);
		AddCommand("Bizarro - Map 18", "idclev 18", true);
		AddCommand("The War Rooms - Map 19", "idclev 19", true);
		AddCommand("Intruder Alert! - Map 20", "idclev 20", true);
		AddInfo("");
		AddInfo("HARMONY");
		AddCommand("Abduction - Map 01", "idclev 01", true);
		AddCommand("Harm's Way - Map 02", "idclev 02", true);
		AddCommand("Operation Rescue - Map 03", "idclev 03", true);
		AddCommand("Megalopolis - Map 04", "idclev 04", true);
		AddCommand("The Hospital - Map 05", "idclev 05", true);
		AddCommand("The Weapons Factory - Map 06", "idclev 06", true);
		AddCommand("The Underwater Lab - Map 07", "idclev 07", true);
		AddCommand("Airstrip One - Map 08", "idclev 08", true);
		AddCommand("The Launch Base - Map 09", "idclev 09", true);
		AddCommand("The Radioactive Zone - Map 10", "idclev 10", true);
		AddCommand("Echidna - Map 11", "idclev 11", true);
	}

	private void BuildChexItems()
	{
		AddCommand("Mini Zorcher", "summon MiniZorcher", true);
		AddCommand("Super Bootspork", "summon SuperBootspork", true);
		AddCommand("Large Zorcher", "summon LargeZorcher", true);
		AddCommand("Rapid Zorcher", "summon RapidZorcher", true);
		AddCommand("Zorch Propulsor", "summon ZorchPropulsor", true);
		AddCommand("Phasing Zorcher", "summon PhasingZorcher", true);
		AddCommand("LAZ Device", "summon LAZDevice", true);
		AddCommand("Mini Zorcher Pack", "summon MiniZorchPack", true);
		AddCommand("Mini Zorcher Recharge", "summon MiniZorchRecharge", true);
		AddCommand("Large Zorcher Pack", "summon LargeZorchPack", true);
		AddCommand("Large Zorcher Recharge", "summon LargeZorchRecharge", true);
		AddCommand("Zorch Propulsor Recharge", "summon PropulsorZorch", true);
		AddCommand("Zorch Propulsor Pack", "summon PropulsorZorchPack", true);
		AddCommand("Phasing Zorcher Recharge", "summon PhasingZorch", true);
		AddCommand("Phasing Zorcher Pack", "summon PhasingZorchPack", true);
		AddCommand("Zorchpack", "summon Zorchpack", true);
		AddCommand("Blue Key", "summon ChexBlueCard", true);
		AddCommand("Red Key", "summon ChexRedCard", true);
		AddCommand("Yellow Key", "summon ChexYellowCard", true);
		AddCommand("Supercharge Breakfast", "summon SuperchargeBreakfast", true);
		AddCommand("Bowl Of Fruit", "summon BowlOfFruit", true);
		AddCommand("Bowl Of Vegetables", "summon BowlOfVegetables", true);
		AddCommand("Glass Of Water", "summon GlassOfWater", true);
		AddCommand("Super Chex Armor", "summon SuperChexArmor", true);
		AddCommand("Chex Armor", "summon ChexArmor", true);
		AddCommand("Computer Area Map", "summon ComputerAreaMap", true);
		AddCommand("Slime Proof Suit", "summon SlimeProofSuit", true);
		AddCommand("Slime Repellent", "summon SlimeRepellent", true);
	}

	private void BuildChexMonsters()
	{
		AddCommand("Armored Flemoid", "summon ArmoredFlemoidusBipedicus", true);
		AddCommand("Invisible enemy", "summon ChexSoul", true);
		AddCommand("The Flembrane", "summon Flembrane", true);
		AddCommand("Bipedal Flemoid", "summon FlemoidusBipedicus", true);
		AddCommand("Common Flemoid", "summon FlemoidusCommonus", true);
		AddCommand("Cycloptic Flemoid", "summon FlemoidusCycloptisCommonus", true);
	}

	private void BuildChexLevels()
	{
		AddInfo("CHEX QUEST");
		AddCommand("Landing Zone - E1M1", "idclev 11", true);
		AddCommand("Storage Facility - E1M2", "idclev 12", true);
		AddCommand("Laboratory - E1M3", "idclev 13", true);
		AddCommand("Arboretum - E1M4", "idclev 14", true);
		AddCommand("Caverns of Bazoik - E1M5", "idclev 15", true);
		AddInfo("");
		AddInfo("CHEX QUEST 2");
		AddCommand("Space Port - E2M1", "idclev 21", true);
		AddCommand("Cinema - E2M2", "idclev 22", true);
		AddCommand("Chex Museum - E2M3", "idclev 23", true);
		AddCommand("City Streets - E2M4", "idclev 24", true);
		AddCommand("Sewer System - E2M5", "idclev 25", true);
		AddInfo("");
		AddInfo("CHEX QUEST 3");
		AddCommand("Central Command - E3M1", "idclev 31", true);
		AddCommand("United Cereals - E3M2", "idclev 32", true);
		AddCommand("Villa Chex - E3M3", "idclev 33", true);
		AddCommand("Provincial Park - E3M4", "idclev 34", true);
		AddCommand("Meteor Spaceship - E3M5", "idclev 35", true);
	}

	private void BuildHereticItems()
	{
		AddCommand("Elvenwand", "summon Goldwand", true);
		AddCommand("Gauntlets of the Necromancer", "summon Gauntlets", true);
		AddCommand("Ethereal Crossbow", "summon Crossbow", true);
		AddCommand("Dragon Claw", "summon Blaster", true);
		AddCommand("Phoenix Rod", "summon PhoenixRod", true);
		AddCommand("Hellstaff", "summon SkullRod", true);
		AddCommand("Firemace", "summon Mace", true);
		AddCommand("Blue Key", "summon KeyBlue", true);
		AddCommand("Green Key", "summon KeyGreen", true);
		AddCommand("Yellow Key", "summon KeyYellow", true);
		AddCommand("Bag of holding", "summon BagOfHolding", true);
		AddCommand("Mystic Urn", "summon ArtiSuperHealth", true);
		AddCommand("Crystal vial", "summon CrystalVial", true);
		AddCommand("Quartz Flask", "summon ArtiHealth", true);
		AddCommand("Enchanted shield", "summon EnchantedShield", true);
		AddCommand("Silver shield", "summon SilverShield", true);
		AddCommand("Map scroll", "summon SuperMap", true);
		AddCommand("Morph Ovum", "summon ArtiEgg", true);
		AddCommand("Wings of Wrath", "summon ArtiFly", true);
		AddCommand("Shadow Sphere", "summon ArtiInvisibility", true);
		AddCommand("Ring of Invincibility", "summon ArtiInvulnerability", true);
		AddCommand("Chaos Device", "summon ArtiTeleport", true);
		AddCommand("Time Bomb of the Ancients", "summon ArtiTimeBomb", true);
		AddCommand("Tome of Power", "summon ArtiTomeOfPower", true);
		AddCommand("Torch", "summon ArtiTorch", true);
	}

	private void BuildHereticMonsters()
	{
		AddCommand("Weredragon", "summon Beast", true);
		AddCommand("Chicken", "summon Chicken", true);
		AddCommand("Sabreclaw", "summon Clink", true);
		AddCommand("Iron lich", "summon Ironlich", true);
		AddCommand("Undead warrior", "summon Knight", true);
		AddCommand("Ghost warrior", "summon KnightGhost", true);
		AddCommand("Maulotaur", "summon Minotaur", true);
		AddCommand("Golem", "summon Mummy", true);
		AddCommand("Ghost golem", "summon MummyGhost", true);
		AddCommand("Nitrogolem", "summon MummyLeader", true);
		AddCommand("Ghost nitrogolem", "summon MummyLeaderGhost", true);
		AddCommand("Ophidian", "summon Snake", true);
		AddCommand("Disciple of D'Sparil", "summon Wizard", true);
		AddCommand("Gargoyle", "summon HereticImp", true);
		AddCommand("Fire Gargoyle", "summon HereticImpLeader", true);
	}

	private void BuildHereticLevels()
	{
		AddInfo("HERETIC: SHADOW OF THE SERPENT RIDERS");
		AddCommand("The Docks - E1M1", "idclev 11", true);
		AddCommand("The Dungeons - E1M2", "idclev 12", true);
		AddCommand("The Gatehouse - E1M3", "idclev 13", true);
		AddCommand("The Guard Tower - E1M4", "idclev 14", true);
		AddCommand("The Citadel - E1M5", "idclev 15", true);
		AddCommand("The Cathedral - E1M6", "idclev 16", true);
		AddCommand("The Crypts - E1M7", "idclev 17", true);
		AddCommand("Hell's Maw - E1M8", "idclev 18", true);
		AddCommand("The Graveyard - E1M9", "idclev 19", true);
		AddInfo("");
		AddCommand("The Crater - E2M1", "idclev 21", true);
		AddCommand("The Lava Pits - E2M2", "idclev 22", true);
		AddCommand("The River of Fire - E2M3", "idclev 23", true);
		AddCommand("The Ice Grotto - E2M4", "idclev 24", true);
		AddCommand("The Catacombs - E2M5", "idclev 25", true);
		AddCommand("The Labyrinth - E2M6", "idclev 26", true);
		AddCommand("The Great Hall - E2M7", "idclev 27", true);
		AddCommand("The Portals of Chaos - E2M8", "idclev 28", true);
		AddCommand("The Glacier - E2M9", "idclev 29", true);
		AddInfo("");
		AddCommand("The Storehouse - E3M1", "idclev 31", true);
		AddCommand("The Cesspool - E3M2", "idclev 32", true);
		AddCommand("The Confluence - E3M3", "idclev 33", true);
		AddCommand("The Azure Fortress - E3M4", "idclev 34", true);
		AddCommand("The Ophidian Lair - E3M5", "idclev 35", true);
		AddCommand("The Halls of Fear - E3M6", "idclev 36", true);
		AddCommand("The Chasm - E3M7", "idclev 37", true);
		AddCommand("D'Sparil's Keep - E3M8", "idclev 38", true);
		AddCommand("The Aquifer - E3M9", "idclev 39", true);
		AddInfo("");
		AddCommand("Catafalque - E4M1", "idclev 41", true);
		AddCommand("Blockhouse - E4M2", "idclev 42", true);
		AddCommand("Ambulatory - E4M3", "idclev 43", true);
		AddCommand("Sepulcher - E4M4", "idclev 44", true);
		AddCommand("Great Stair - E4M5", "idclev 45", true);
		AddCommand("Halls of the Apostate - E4M6", "idclev 46", true);
		AddCommand("Ramparts of Perdition - E4M7", "idclev 47", true);
		AddCommand("Shattered Bridge - E4M8", "idclev 48", true);
		AddCommand("Mausoleum - E4M9", "idclev 49", true);
		AddInfo("");
		AddCommand("Ochre Cliffs - E5M1", "idclev 51", true);
		AddCommand("Rapids - E5M2", "idclev 52", true);
		AddCommand("Quay - E5M3", "idclev 53", true);
		AddCommand("Courtyard - E5M4", "idclev 54", true);
		AddCommand("Hydratyr - E5M5", "idclev 55", true);
		AddCommand("Colonnade - E5M6", "idclev 56", true);
		AddCommand("Foetid Manse - E5M7", "idclev 57", true);
		AddCommand("Field of Judgement - E5M8", "idclev 58", true);
		AddCommand("Skein of D'Sparil - E5M9", "idclev 59", true);
		AddInfo("");
		AddCommand("Raven's Lair - E6M1", "idclev 61", true);
		AddCommand("Water Shrine - E6M2", "idclev 62", true);
		AddCommand("American's Legacy - E6M3", "idclev 63", true);
	}

	private void BuildHexenItems()
	{
		AddCommand("Timons axe", "summon FWeapAxe", true);
		AddCommand("Firestorm", "summon CWeapFlame", true);
		AddCommand("Frost Shards", "summon MWeapFrost", true);
		AddCommand("Hammer of retribution", "summon FWeapHammer", true);
		AddCommand("Serpent staff", "summon CWeapStaff", true);
		AddCommand("Arc of death", "summon MWeapLightning", true);
		AddCommand("Quietus", "summon FWeapQuietus", true);
		AddCommand("Wraithverge", "summon CWeapWraithverge", true);
		AddCommand("Bloodscourge", "summon MWeapBloodscourge", true);
		AddCommand("Axe Key", "summon KeyAxe", true);
		AddCommand("Castle Key", "summon KeyCastle", true);
		AddCommand("Cave Key", "summon KeyCave", true);
		AddCommand("Dungeon Key", "summon KeyDungeon", true);
		AddCommand("Emerald Key", "summon KeyEmerald", true);
		AddCommand("Fire Key", "summon KeyFire", true);
		AddCommand("Horn Key", "summon KeyHorn", true);
		AddCommand("Rusted Key", "summon KeyRusted", true);
		AddCommand("Silver Key", "summon KeySilver", true);
		AddCommand("Steel Key", "summon KeySteel", true);
		AddCommand("Swamp Key", "summon KeySwamp", true);
		AddCommand("Mystic Urn", "summon ArtiSuperHealth", true);
		AddCommand("Crystal vial", "summon CrystalVial", true);
		AddCommand("Quartz flask", "summon ArtiHealth", true);
		AddCommand("Amulet of Warding", "summon AmuletOfWarding", true);
		AddCommand("Mesh Armor", "summon MeshArmor", true);
		AddCommand("Falcon Shield", "summon FalconShield", true);
		AddCommand("Platinum Helm", "summon PlatinumHelm", true);
		AddCommand("Disc of repulsion", "summon ArtiBlastRadius", true);
		AddCommand("Dragonskin bracers", "summon ArtiBoostArmor", true);
		AddCommand("Krater of might", "summon ArtiBoostMana", true);
		AddCommand("Dark servant artifact", "summon ArtiDarkServant", true);
		AddCommand("Wings of wrath", "summon ArtiFly", true);
		AddCommand("Icon of the defender", "summon ArtiInvulnerability2", true);
		AddCommand("Torch", "summon ArtiTorch", true);
		AddCommand("Flechette", "summon ArtiPoisonBag", true);
		AddCommand("Porkalator", "summon ArtiPork", true);
		AddCommand("Boots of speed artifact", "summon ArtiSpeedBoots", true);
		AddCommand("Chaos device", "summon ArtiTeleport", true);
	}

	private void BuildHexenMonsters()
	{
		AddCommand("Ettin", "summon Ettin", true);
		AddCommand("Phantasmal ettin", "summon EttinMash", true);
		AddCommand("Afrit", "summon FireDemon", true);
		AddCommand("Centaur", "summon Centaur", true);
		AddCommand("Slaughtaur", "summon CentaurLeader", true);
		AddCommand("Phantasmal centaur", "summon CentaurMash", true);
		AddCommand("Green chaos serpent", "summon Demon1", true);
		AddCommand("Brown chaos serpent", "summon Demon2", true);
		AddCommand("Dark bishop", "summon bishop", true);
		AddCommand("Wendigo", "summon IceGuy", true);
		AddCommand("Stalker", "summon Serpent", true);
		AddCommand("Death wyvern", "summon Dragon", true);
		AddCommand("Traductus", "summon ClericBoss", true);
		AddCommand("Zedek", "summon FighterBoss", true);
		AddCommand("Menelkir", "summon MageBoss", true);
		AddCommand("Heresiarch", "summon Heresiarch", true);
		AddCommand("Korax", "summon Korax", true);
	}

	private void BuildHexenLevels()
	{
		AddInfo("HEXEN: BEYOND HERETIC");
		AddCommand("Winnowing Hall - Map 01", "idclev 01", true);
		AddCommand("Seven Portals - Map 02", "idclev 02", true);
		AddCommand("Guardian of Ice - Map 03", "idclev 03", true);
		AddCommand("Guardian of Fire - Map 04", "idclev 04", true);
		AddCommand("Guardian of Steel - Map 05", "idclev 05", true);
		AddCommand("Bright Crucible - Map 06", "idclev 06", true);
		AddCommand("Shadow Wood - Map 07", "idclev 07", true);
		AddCommand("Darkmere - Map 08", "idclev 08", true);
		AddCommand("Caves of Circe - Map 09", "idclev 09", true);
		AddCommand("Wastelands - Map 10", "idclev 10", true);
		AddCommand("Sacred Grove - Map 11", "idclev 11", true);
		AddCommand("Hypostyle - Map 12", "idclev 12", true);
		AddCommand("Heresiarch's Seminary - Map 13", "idclev 13", true);
		AddCommand("Dragon Chapel - Map 14", "idclev 14", true);
		AddCommand("Griffin Chapel - Map 15", "idclev 15", true);
		AddCommand("Deathwind Chapel - Map 16", "idclev 16", true);
		AddCommand("Orchard of Lamentations - Map 17", "idclev 17", true);
		AddCommand("Silent Refectory - Map 18", "idclev 18", true);
		AddCommand("Wolf Chapel - Map 19", "idclev 19", true);
		AddCommand("Forsaken Outpost - Map 20", "idclev 20", true);
		AddCommand("Castle of Grief - Map 21", "idclev 21", true);
		AddCommand("Gibbet - Map 22", "idclev 22", true);
		AddCommand("Effluvium - Map 23", "idclev 23", true);
		AddCommand("Dungeon - Map 24", "idclev 24", true);
		AddCommand("Desolate Garden - Map 25", "idclev 25", true);
		AddCommand("Necropolis - Map 26", "idclev 26", true);
		AddCommand("Zedek's Tomb - Map 27", "idclev 27", true);
		AddCommand("Menelkir's Tomb - Map 28", "idclev 28", true);
		AddCommand("Traductus' Tomb - Map 29", "idclev 29", true);
		AddCommand("Vivarium - Map 30", "idclev 30", true);
		AddCommand("Dark Crucible - Map 31", "idclev 31", true);
	}

	private void BuildStrifeItems()
	{
		AddCommand("Fire Crossbow", "summon StrifeCrossbow", true);
		AddCommand("Poison Crossbow", "summon StrifeCrossbow2", true);
		AddCommand("Assault Gun", "summon AssaultGun", true);
		AddCommand("Mini Missile Launcher", "summon MiniMissileLauncher", true);
		AddCommand("Flame Thrower", "summon FlameThrower", true);
		AddCommand("Mauler", "summon Mauler", true);
		AddCommand("Torpedo Mauler", "summon Mauler", true);
		AddCommand("Grenade Launcher", "summon StrifeGrenadeLauncher", true);
		AddCommand("The Sigil", "summon Sigil", true);
		AddCommand("Poison Bolts", "summon PoisonBolts", true);
		AddCommand("Electric Bolts", "summon ElectricBolts", true);
		AddCommand("Bullet ammo", "summon ClipOfBullets", true);
		AddCommand("Box of Bullets", "summon BoxOfBullets", true);
		AddCommand("Mini Missiles", "summon MiniMissiles", true);
		AddCommand("Crate of Missiles", "summon CrateOfMissiles", true);
		AddCommand("HE Grenade", "summon HEGrenadeRounds", true);
		AddCommand("Phosphorus Grenade Ammo", "summon PhosphorusGrenadeRounds", true);
		AddCommand("Cell ammo", "summon EnergyPod", true);
		AddCommand("Cell ammo Pack", "summon EnergyPack", true);
		AddCommand("Ammo Backpack", "summon AmmoSatchel", true);
		AddCommand("Surgery Kit", "summon SurgeryKit", true);
		AddCommand("Medical Kit", "summon MedicalKit", true);
		AddCommand("Med Patch", "summon MedPatch", true);
		AddCommand("Shadow Armor", "summon ShadowArmor", true);
		AddCommand("LeatherArmor", "summon LeatherArmor", true);
		AddCommand("50 Gold coins", "summon Gold50", true);
		AddCommand("Metal Armor", "summon MetalArmor", true);
		AddCommand("Teleporter Beacon", "summon TeleporterBeacon", true);
		AddCommand("Environmental Suit", "summon EnvironmentalSuit", true);
		AddCommand("Targeter", "summon Targeter", true);
		AddCommand("Scanner", "summon Scanner", true);
	}

	private void BuildStrifeMonsters()
	{
		AddCommand("Standard trooper", "summon Acolyte", true);
		AddCommand("Low-rank officer in red uniform", "summon AcolyteRed", true);
		AddCommand("Guard in brown uniform", "summon AcolyteRust", true);
		AddCommand("Elite guard in gold uniform", "summon AcolyteGold", true);
		AddCommand("Overseer in bright green uniform", "summon AcolyteLGreen", true);
		AddCommand("Scanner trooper in teal uniform", "summon AcolyteBlue", true);
		AddCommand("Standard Front soldier", "summon Rebel1", true);
		AddCommand("cybernetic boss", "summon StrifeBishop", true);
		AddCommand("Small floating Robot", "summon Sentinel", true);
		AddCommand("Large, lumbering assault robot", "summon Crusader", true);
		AddCommand("Towering mech", "summon Inquisitor", true);
		AddCommand("Reaver", "summon Reaver", true);
		AddCommand("Templar", "summon Templar", true);
		AddCommand("Stalker", "summon Stalker", true);
	}

	private void BuildStrifeLevels()
	{
		AddInfo("STRIFE: QUEST FOR THE SIGIL");
		AddCommand("Sanctuary [registered version] - Map 01", "idclev 01", true);
		AddCommand("Town [registered version] - Map 02", "idclev 02", true);
		AddCommand("Front Base [registered version] - Map 03", "idclev 03", true);
		AddCommand("Power Station - Map 04", "idclev 04", true);
		AddCommand("Prison - Map 05", "idclev 05", true);
		AddCommand("Sewers - Map 06", "idclev 06", true);
		AddCommand("Castle - Map 07", "idclev 07", true);
		AddCommand("Audience Chamber - Map 08", "idclev 08", true);
		AddCommand("Castle: Programmer's Keep - Map 09", "idclev 09", true);
		AddCommand("New Front Base - Map 10", "idclev 10", true);
		AddCommand("Borderlands - Map 11", "idclev 11", true);
		AddCommand("The Temple of the Oracle - Map 12", "idclev 12", true);
		AddCommand("Catacombs - Map 13", "idclev 13", true);
		AddCommand("Mines - Map 14", "idclev 14", true);
		AddCommand("Fortress: Administration - Map 15", "idclev 15", true);
		AddCommand("Fortress: Bishop's Tower - Map 16", "idclev 16", true);
		AddCommand("Fortress: The Bailey - Map 17", "idclev 17", true);
		AddCommand("Fortress: Stores - Map 18", "idclev 18", true);
		AddCommand("Fortress: Security Complex - Map 19", "idclev 19", true);
		AddCommand("Factory: Receiving - Map 20", "idclev 20", true);
		AddCommand("Factory: Manufacturing - Map 21", "idclev 21", true);
		AddCommand("Factory: Forge - Map 22", "idclev 22", true);
		AddCommand("Order Commons - Map 23", "idclev 23", true);
		AddCommand("Factory: Conversion Chapel - Map 24", "idclev 24", true);
		AddCommand("Catacombs: Ruined Temple - Map 25", "idclev 25", true);
		AddCommand("Proving Grounds - Map 26", "idclev 26", true);
		AddCommand("The Lab - Map 27", "idclev 27", true);
		AddCommand("Alien Ship - Map 28", "idclev 28", true);
		AddCommand("Entity's Lair - Map 29", "idclev 29", true);
		AddCommand("Abandoned Front Base - Map 30", "idclev 30", true);
		AddCommand("Training Facility - Map 31", "idclev 31", true);
		AddCommand("Sanctuary [demo version] - Map 32", "idclev 32", true);
		AddCommand("Town [demo version] - Map 33", "idclev 33", true);
		AddCommand("Movement Base [demo version] - Map 34", "idclev 33", true);
	}

	override void Drawer()
	{
		int rowHeight = RowHeight();
		int rows = VisibleRows();
		int titleY = HEADER_Y;
		int infoY = titleY + mFont.GetHeight() * CleanYfac_1 + 4;
		int listTop = LIST_TOP;
		int cmdWidth = 0;
		for (int i = 0; i < mEntryCommand.Size(); ++i)
		{
			cmdWidth = max(cmdWidth, mFont.StringWidth(mEntryCommand[i]));
		}
		int cmdX = screen.GetWidth() - LIST_RIGHT_PAD - cmdWidth * CleanXfac_1;

		screen.DrawText(mFont, Font.CR_ORANGE, LIST_LEFT, titleY,
			"Cheat Menu - " .. GameTitle() .. " / " .. PageTitle(), DTA_CleanNoMove_1, true);
		screen.DrawText(mFont, Font.CR_BROWN, LIST_LEFT, infoY,
			"Use B or togglecheatmenu to close. Enter activates. Back returns to the main cheat page.", DTA_CleanNoMove_1, true);

		screen.Dim(0, 0.8, 0, listTop - 6, screen.GetWidth(), screen.GetHeight() - listTop + 6);

		for (int i = 0; i < rows; ++i)
		{
			int index = mTop + i;
			if (index >= mEntryText.Size())
			{
				break;
			}

			int y = listTop + i * rowHeight;
			bool selected = (index == mSelected);

			if (selected)
			{
				screen.Dim(Color(160, 120, 0), 0.6, LIST_LEFT - 4, y - 1, screen.GetWidth() - LIST_LEFT * 2 + 8, rowHeight - 1);
			}

			if (!mEntrySelectable[index])
			{
				screen.DrawText(mFont, Font.CR_ORANGE, LIST_LEFT, y, mEntryText[index], DTA_CleanNoMove_1, true);
				continue;
			}

			if (mEntryIsSubmenu[index])
			{
				screen.DrawText(mFont, selected ? Font.CR_GOLD : Font.CR_ORANGE, LIST_LEFT, y, mEntryText[index], DTA_CleanNoMove_1, true);
				screen.DrawText(mFont, selected ? Font.CR_YELLOW : Font.CR_BROWN, cmdX, y, ">", DTA_CleanNoMove_1, true);
			}
			else
			{
				screen.DrawText(mFont, selected ? Font.CR_YELLOW : Font.CR_WHITE, LIST_LEFT, y, mEntryText[index], DTA_CleanNoMove_1, true);
				screen.DrawText(mFont, selected ? Font.CR_GOLD : Font.CR_BROWN, cmdX, y, mEntryCommand[index], DTA_CleanNoMove_1, true);
			}
		}
	}
}

class CheatMenuDoomItems : CheatMenu
{
	override void SetupPage() { mPage = 'doom_items'; mRootPage = false; }
}

class CheatMenuDoomMonsters : CheatMenu
{
	override void SetupPage() { mPage = 'doom_monsters'; mRootPage = false; }
}

class CheatMenuDoomLevels : CheatMenu
{
	override void SetupPage() { mPage = 'doom_levels'; mRootPage = false; }
}

class CheatMenuChexItems : CheatMenu
{
	override void SetupPage() { mPage = 'chex_items'; mRootPage = false; }
}

class CheatMenuChexMonsters : CheatMenu
{
	override void SetupPage() { mPage = 'chex_monsters'; mRootPage = false; }
}

class CheatMenuChexLevels : CheatMenu
{
	override void SetupPage() { mPage = 'chex_levels'; mRootPage = false; }
}

class CheatMenuHereticItems : CheatMenu
{
	override void SetupPage() { mPage = 'heretic_items'; mRootPage = false; }
}

class CheatMenuHereticMonsters : CheatMenu
{
	override void SetupPage() { mPage = 'heretic_monsters'; mRootPage = false; }
}

class CheatMenuHereticLevels : CheatMenu
{
	override void SetupPage() { mPage = 'heretic_levels'; mRootPage = false; }
}

class CheatMenuHexenItems : CheatMenu
{
	override void SetupPage() { mPage = 'hexen_items'; mRootPage = false; }
}

class CheatMenuHexenMonsters : CheatMenu
{
	override void SetupPage() { mPage = 'hexen_monsters'; mRootPage = false; }
}

class CheatMenuHexenLevels : CheatMenu
{
	override void SetupPage() { mPage = 'hexen_levels'; mRootPage = false; }
}

class CheatMenuStrifeItems : CheatMenu
{
	override void SetupPage() { mPage = 'strife_items'; mRootPage = false; }
}

class CheatMenuStrifeMonsters : CheatMenu
{
	override void SetupPage() { mPage = 'strife_monsters'; mRootPage = false; }
}

class CheatMenuStrifeLevels : CheatMenu
{
	override void SetupPage() { mPage = 'strife_levels'; mRootPage = false; }
}
