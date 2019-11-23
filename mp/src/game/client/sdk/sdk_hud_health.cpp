//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//
#include "cbase.h"
#include "hud.h"
#include "hud_macros.h"
#include "view.h"
#include "iclientmode.h"
#include <KeyValues.h>
#include <vgui/ISurface.h>
#include <vgui/ISystem.h>
#include <vgui_controls/AnimationController.h>
#include "hudelement.h"
#include "hud_numericdisplay.h"
#include "c_sdk_player.h"

using namespace vgui;

//-----------------------------------------------------------------------------
// Purpose: Health panel
//-----------------------------------------------------------------------------
class CHudHealth : public CHudElement, public CHudNumericDisplay
{
	DECLARE_CLASS_SIMPLE( CHudHealth, CHudNumericDisplay );

public:
	CHudHealth( const char *pElementName );

	virtual void Init( void );
	virtual void VidInit( void ) {}
	virtual void Reset( void );
	virtual void OnThink();
	virtual void Paint( void );
	virtual void ApplySchemeSettings( IScheme *scheme );

private:
	// old variables
	int m_iHealth;

	CHudTexture *m_pHealthIcon;

	CPanelAnimationVarAliasType( float, icon_xpos, "icon_xpos", "0", "proportional_float" );
	CPanelAnimationVarAliasType( float, icon_ypos, "icon_ypos", "2", "proportional_float" );

	float icon_tall;
	float icon_wide;

};

DECLARE_HUDELEMENT( CHudHealth );

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------
CHudHealth::CHudHealth( const char *pElementName ) : CHudElement( pElementName ), CHudNumericDisplay( NULL, "HudHealth" )
{
	SetHiddenBits( HIDEHUD_HEALTH | HIDEHUD_PLAYERDEAD | HIDEHUD_NEEDSUIT);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CHudHealth::Init()
{
	m_iHealth		= 100;

	icon_tall		= 0;
	icon_wide		= 0;

	SetDisplayValue( m_iHealth );
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CHudHealth::ApplySchemeSettings( IScheme *scheme )
{
	BaseClass::ApplySchemeSettings( scheme );

	if( !m_pHealthIcon )
		m_pHealthIcon = gHUD.GetIcon( "item_healthkit" );

	if( m_pHealthIcon )
	{
		icon_tall = GetTall() - YRES(2);
		float scale = icon_tall / (float)m_pHealthIcon->Height();
		icon_wide = ( scale ) * (float)m_pHealthIcon->Width();
	}
}

//-----------------------------------------------------------------------------
// Purpose: reset health to normal color at round restart
//-----------------------------------------------------------------------------
void CHudHealth::Reset()
{
	g_pClientMode->GetViewportAnimationController()->StartAnimationSequence( "HealthRestored" );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CHudHealth::OnThink()
{
	int realHealth = 0;
	auto *local = C_BasePlayer::GetLocalPlayer();
	if ( local )
		// Never below zero
		realHealth = max( local->GetHealth(), 0 );

	// Only update the fade if we've changed health
	if ( realHealth == m_iHealth )
		return;

	if( realHealth > m_iHealth)
		// round restarted, we have 100 again
		g_pClientMode->GetViewportAnimationController()->StartAnimationSequence( "HealthRestored" );
	else if ( realHealth <= 25 )
		// we are badly injured
		g_pClientMode->GetViewportAnimationController()->StartAnimationSequence( "HealthLow" );
	else if( realHealth < m_iHealth )
		// took a hit
		g_pClientMode->GetViewportAnimationController()->StartAnimationSequence( "HealthTookDamage" );

	m_iHealth = realHealth;

	SetDisplayValue( m_iHealth );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CHudHealth::Paint( void )
{
	if( m_pHealthIcon )
		m_pHealthIcon->DrawSelf( icon_xpos, icon_ypos, icon_wide, icon_tall, GetFgColor() );

	//draw the health icon
	BaseClass::Paint();
}