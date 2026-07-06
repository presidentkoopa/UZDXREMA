class OtherPlayerTagVisual : VisualThinker
{
	const TAG_CANVAS_WIDTH = 1024;
	const TAG_CANVAS_HEIGHT = 256;
	const TAG_TEXT_SCALE = 8.0;
	const TAG_BAR_LEFT = 96;
	const TAG_BAR_TOP = 172;
	const TAG_BAR_WIDTH = 832;
	const TAG_BAR_HEIGHT = 40;
	const TAG_SCALE_VR_X = 0.34;
	const TAG_SCALE_VR_Y = 0.67;
	const TAG_SCALE_NONVR = 0.47;
	const TAG_Z_OFFSET_VR = -29;
	const TAG_Z_OFFSET_NONVR = -4;

	int TrackedPlayer;
	String CanvasName;
	String CachedName;
	int CachedHealth;
	int CachedMaxHealth;
	int CachedStateMask;

	void InitForPlayer(int playerNumber, TextureID textureId, String textureName)
	{
		TrackedPlayer = playerNumber;
		CanvasName = textureName;
		CachedHealth = -1;
		CachedMaxHealth = -1;
		CachedStateMask = -1;
		Texture = textureId;
		Scale = (0.35, 0.35);
		Offset = (0, 0);
		Flags |= SPF_FULLBRIGHT | SPF_FACECAMERA;
		LightLevel = 255;
		VisualThinkerFlags |= VTF_FlipY;
		Alpha = 0;
		SetRenderStyle(STYLE_Translucent);
		UpdateSpriteInfo();
	}

	void SetVisible(bool visible)
	{
		Alpha = visible ? 1.0 : 0.0;
	}

	void Sync(PlayerInfo info, bool showName, bool showHealth, bool isLocalVR)
	{
		if (info.mo == null)
		{
			SetVisible(false);
			return;
		}

		double crouchComp = info.mo.player != null ? max(0.0, -info.mo.player.crouchoffset) : 0.0;
		Pos = info.mo.Pos + (0, 0, info.mo.Height - info.mo.Floorclip + crouchComp);
		if (isLocalVR)
		{
			Scale = (TAG_SCALE_VR_X, TAG_SCALE_VR_Y);
			Offset = (0, 0);
			Flags = (Flags | SPF_NOFACECAMERA) & ~SPF_FACECAMERA;
			Pos.Z += TAG_Z_OFFSET_VR;
		}
		else
		{
			Scale = (TAG_SCALE_NONVR, TAG_SCALE_NONVR);
			Offset = (0, 0);
			Flags = (Flags | SPF_FACECAMERA) & ~SPF_NOFACECAMERA;
			Pos.Z += TAG_Z_OFFSET_NONVR;
		}
		UpdateSector();

		let stateMask = (showName ? 1 : 0) | (showHealth ? 2 : 0);
		SetVisible(stateMask != 0);
		if (stateMask == 0)
		{
			return;
		}

		int maxHealth = max(1, info.mo.GetMaxHealth(true));
		int health = clamp(info.mo.health, 0, maxHealth);
		String displayName = info.GetUserName();

		bool forceRedraw = isLocalVR;
		if (!forceRedraw &&
			displayName == CachedName &&
			health == CachedHealth &&
			maxHealth == CachedMaxHealth &&
			stateMask == CachedStateMask)
		{
			return;
		}

		if (Redraw(displayName, health, maxHealth, showName, showHealth))
		{
			CachedName = displayName;
			CachedHealth = health;
			CachedMaxHealth = maxHealth;
			CachedStateMask = stateMask;
		}
	}

	bool Redraw(String displayName, int health, int maxHealth, bool showName, bool showHealth)
	{
		let canvas = TexMan.GetCanvas(CanvasName);
		if (canvas == null)
		{
			return false;
		}

		TexMan.SetCanvasTextureTranslucent(CanvasName, true);

		if (showName)
		{
			int textWidth = int(SmallFont.StringWidth(displayName, false) * TAG_TEXT_SCALE);
			int textX = max(0, (TAG_CANVAS_WIDTH - textWidth) / 2);
			canvas.DrawText(SmallFont, Font.CR_UNTRANSLATED, textX, 8, displayName, DTA_ScaleX, TAG_TEXT_SCALE, DTA_ScaleY, TAG_TEXT_SCALE);
		}

		if (showHealth)
		{
			int fillWidth = TAG_BAR_WIDTH * health / maxHealth;
			int barRight = TAG_BAR_LEFT + TAG_BAR_WIDTH;
			int barBottom = TAG_BAR_TOP + TAG_BAR_HEIGHT;

			canvas.DrawLineFrame(0xFF000000, TAG_BAR_LEFT - 1, TAG_BAR_TOP - 1, TAG_BAR_WIDTH + 2, TAG_BAR_HEIGHT + 2);
			canvas.Clear(TAG_BAR_LEFT, TAG_BAR_TOP, barRight, barBottom, 0xFF4A1010);
			if (fillWidth > 0)
			{
				canvas.Clear(TAG_BAR_LEFT, TAG_BAR_TOP, TAG_BAR_LEFT + fillWidth, barBottom, 0xFF58C43A);
			}
		}

		UpdateSpriteInfo();
		return true;
	}
}

class OtherPlayerTagHandler : EventHandler
{
	OtherPlayerTagVisual Tags[MAXPLAYERS];
	CVar NamesCVar;
	CVar HealthCVar;
	CVar VRModeCVar;

	override void OnRegister()
	{
		NamesCVar = CVar.FindCVar('cl_otherplayernames');
		HealthCVar = CVar.FindCVar('cl_otherplayerhealth');
		VRModeCVar = CVar.FindCVar('vr_mode');
		MarkCanvasTextures();
	}

	override void WorldLoaded(WorldEvent e)
	{
		ClearTags();
		MarkCanvasTextures();
	}

	override void WorldUnloaded(WorldEvent e)
	{
		ClearTags();
	}

	override void PlayerDisconnected(PlayerEvent e)
	{
		DestroyTag(e.PlayerNumber);
	}

	override void WorldTick()
	{
		if (!multiplayer || consoleplayer < 0 || players[consoleplayer].camera == null)
		{
			ClearTags();
			return;
		}

		for (int i = 0; i < MAXPLAYERS; i++)
		{
			if (i == consoleplayer || !playeringame[i] || players[i].mo == null || players[i].mo.health <= 0)
			{
				DestroyTag(i);
				continue;
			}

			bool showName = ShouldShowForMode(GetModeValue(NamesCVar), players[i].mo);
			bool showHealth = ShouldShowForMode(GetModeValue(HealthCVar), players[i].mo);
			let tag = EnsureTag(i);
			if (tag != null)
			{
				tag.Sync(players[i], showName, showHealth, IsLocalVR());
			}
		}
	}

	int GetModeValue(CVar cvar)
	{
		return cvar != null ? clamp(cvar.GetInt(), 0, 2) : 2;
	}

	bool IsLocalVR()
	{
		return VRModeCVar != null && VRModeCVar.GetInt() != 0;
	}

	bool ShouldShowForMode(int mode, Actor target)
	{
		if (mode <= 0 || target == null)
		{
			return false;
		}
		if (mode >= 2)
		{
			return true;
		}

		let viewer = players[consoleplayer].camera;
		return viewer != null && viewer.AimTarget() == target;
	}

	OtherPlayerTagVisual EnsureTag(int playerNumber)
	{
		let existing = Tags[playerNumber];
		if (existing != null && !existing.bDestroyed)
		{
			return existing;
		}

		String textureName = String.Format("OPLTAG%02d", playerNumber);
		if (IsLocalVR())
		{
			textureName = String.Format("OPLVR%02d", playerNumber);
		}
		TextureID textureId = TexMan.CheckForTexture(textureName, TexMan.Type_Wall);
		if (!textureId.IsValid())
		{
			return null;
		}

		let spawned = VisualThinker.Spawn((class<VisualThinker>)("OtherPlayerTagVisual"), textureId, players[playerNumber].mo.Pos);
		let tag = OtherPlayerTagVisual(spawned);
		if (tag != null)
		{
			tag.InitForPlayer(playerNumber, textureId, textureName);
			Tags[playerNumber] = tag;
		}
		return tag;
	}

	void DestroyTag(int playerNumber)
	{
		if (playerNumber < 0 || playerNumber >= MAXPLAYERS)
		{
			return;
		}

		let tag = Tags[playerNumber];
		if (tag != null && !tag.bDestroyed)
		{
			tag.Destroy();
		}
		Tags[playerNumber] = null;
	}

	void ClearTags()
	{
		for (int i = 0; i < MAXPLAYERS; i++)
		{
			DestroyTag(i);
		}
	}

	void MarkCanvasTextures()
	{
		for (int i = 0; i < MAXPLAYERS; i++)
		{
			TexMan.SetCanvasTextureTranslucent(String.Format("OPLTAG%02d", i), true);
			TexMan.SetCanvasTextureTranslucent(String.Format("OPLVR%02d", i), true);
		}
	}
}
