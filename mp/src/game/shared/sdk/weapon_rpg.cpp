//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "cbase.h"
#include "npcevent.h"
#include "in_buttons.h"
#include "weapon_rpg.h"

#ifdef CLIENT_DLL
	#include "c_sdk_player.h"
	#include "model_types.h"
	#include "beamdraw.h"
	#include "fx_line.h"
	#include "view.h"
#else
	#include "basecombatcharacter.h"
	#include "movie_explosion.h"
	#include "soundent.h"
	#include "player.h"
	#include "rope.h"
	#include "vstdlib/random.h"
	#include "engine/IEngineSound.h"
	#include "explode.h"
	#include "util.h"
	#include "in_buttons.h"
	#include "shake.h"
	#include "te_effect_dispatch.h"
	#include "triggers.h"
	#include "smoke_trail.h"
	#include "collisionutils.h"
	#include "sdk_shareddefs.h"
	#include "weapon_sdkbase.h"
	#include "basecombatweapon_shared.h"
#endif

#include "debugoverlay_shared.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#define	RPG_SPEED	1500

#ifndef CLIENT_DLL
const char *g_pLaserDotThink = "LaserThinkContext";

static ConVar sk_apc_missile_damage("sk_apc_missile_damage", "15");
#define APC_MISSILE_DAMAGE	sk_apc_missile_damage.GetFloat()

#endif

#ifdef CLIENT_DLL
#define CLaserDot C_LaserDot
#endif

//-----------------------------------------------------------------------------
// Laser Dot
//-----------------------------------------------------------------------------
class CLaserDot : public CBaseEntity
{
	DECLARE_CLASS( CLaserDot, CBaseEntity );
public:

	CLaserDot( void );
	~CLaserDot( void );

	static CLaserDot *Create( const Vector &origin, CBaseEntity *pOwner = NULL, bool bVisibleDot = true );

	void	SetTargetEntity( CBaseEntity *pTarget ) { m_hTargetEnt = pTarget; }
	CBaseEntity *GetTargetEntity( void ) { return m_hTargetEnt; }

	void	SetLaserPosition( const Vector &origin, const Vector &normal );
	Vector	GetChasePosition();
	void	TurnOn( void );
	void	TurnOff( void );
	bool	IsOn() const { return m_bIsOn; }

	void	Toggle( void );

	int		ObjectCaps() { return (BaseClass::ObjectCaps() & ~FCAP_ACROSS_TRANSITION) | FCAP_DONT_SAVE; }

	void	MakeInvisible( void );

#ifdef CLIENT_DLL

	virtual bool			IsTransparent( void ) { return true; }
	virtual RenderGroup_t	GetRenderGroup( void ) { return RENDER_GROUP_TRANSLUCENT_ENTITY; }
	virtual int				DrawModel( int flags );
	virtual void			OnDataChanged( DataUpdateType_t updateType );
	virtual bool			ShouldDraw( void ) { return (IsEffectActive(EF_NODRAW)==false); }

	CMaterialReference	m_hSpriteMaterial;
#endif

protected:
	Vector				m_vecSurfaceNormal;
	EHANDLE				m_hTargetEnt;
	bool				m_bVisibleLaserDot;
	bool				m_bIsOn;

	DECLARE_NETWORKCLASS();
	DECLARE_DATADESC();
public:
	CLaserDot			*m_pNext;
};

IMPLEMENT_NETWORKCLASS_ALIASED( LaserDot, DT_LaserDot )

BEGIN_NETWORK_TABLE( CLaserDot, DT_LaserDot )
END_NETWORK_TABLE()

#ifndef CLIENT_DLL

// a list of laser dots to search quickly
CEntityClassList<CLaserDot> g_LaserDotList;
template <> CLaserDot *CEntityClassList<CLaserDot>::m_pClassList = NULL;
CLaserDot *GetLaserDotList()
{
	return g_LaserDotList.m_pClassList;
}

BEGIN_DATADESC( CMissile )

	DEFINE_FIELD( m_hOwner,					FIELD_EHANDLE ),
	DEFINE_FIELD( m_hRocketTrail,			FIELD_EHANDLE ),
	DEFINE_FIELD( m_flAugerTime,			FIELD_TIME ),
	DEFINE_FIELD( m_flMarkDeadTime,			FIELD_TIME ),
	DEFINE_FIELD( m_flGracePeriodEndsAt,	FIELD_TIME ),
	DEFINE_FIELD( m_flDamage,				FIELD_FLOAT ),
	
	// Function Pointers
	DEFINE_FUNCTION( MissileTouch ),
	DEFINE_FUNCTION( AccelerateThink ),
	DEFINE_FUNCTION( AugerThink ),
	DEFINE_FUNCTION( IgniteThink ),
	DEFINE_FUNCTION( SeekThink ),

END_DATADESC()

LINK_ENTITY_TO_CLASS( rpg_missile, CMissile );

class CWeaponRPG;

//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CMissile::CMissile()
{
	m_hRocketTrail = NULL;
}

CMissile::~CMissile()
{
}


//-----------------------------------------------------------------------------
// Purpose: 
//
//
//-----------------------------------------------------------------------------
void CMissile::Precache( void )
{
	PrecacheModel( "models/weapons/w_missile.mdl" );
	PrecacheModel( "models/weapons/w_missile_launch.mdl" );
	PrecacheModel( "models/weapons/w_missile_closed.mdl" );
}


//-----------------------------------------------------------------------------
// Purpose: 
//
//
//-----------------------------------------------------------------------------
void CMissile::Spawn( void )
{
	Precache();

	//DumbFire();
	SetSolid( SOLID_BBOX );
	SetModel("models/weapons/w_missile_launch.mdl");
	UTIL_SetSize( this, -Vector(4,4,4), Vector(4,4,4) );

	SetTouch( &CMissile::MissileTouch );

	SetMoveType( MOVETYPE_FLYGRAVITY, MOVECOLLIDE_FLY_BOUNCE );
	SetThink( &CMissile::IgniteThink );
	
	SetNextThink( gpGlobals->curtime + 0.3f );

	m_takedamage = DAMAGE_NO;
	m_iHealth = m_iMaxHealth = 100;
	m_bloodColor = DONT_BLEED;
	m_flGracePeriodEndsAt = 0;

	AddFlag( FL_OBJECT );
}


//---------------------------------------------------------
//---------------------------------------------------------
void CMissile::Event_Killed( const CTakeDamageInfo &info )
{
	m_takedamage = DAMAGE_NO;

	ShotDown();
}

unsigned int CMissile::PhysicsSolidMaskForEntity( void ) const
{ 
	return BaseClass::PhysicsSolidMaskForEntity() | CONTENTS_HITBOX;
}

//---------------------------------------------------------
//---------------------------------------------------------
int CMissile::OnTakeDamage_Alive( const CTakeDamageInfo &info )
{
	if ( ( info.GetDamageType() & ( DMG_AIRBOAT) ) == false )
		return 0;

	bool bIsDamaged;
	if( m_iHealth <= AugerHealth() )
	{
		// This missile is already damaged (i.e., already running AugerThink)
		bIsDamaged = true;
	}
	else
	{
		// This missile isn't damaged enough to wobble in flight yet
		bIsDamaged = false;
	}
	
	int nRetVal = BaseClass::OnTakeDamage_Alive( info );

	if( !bIsDamaged )
	{
		if ( m_iHealth <= AugerHealth() )
		{
			ShotDown();
		}
	}

	return nRetVal;
}


//-----------------------------------------------------------------------------
// Purpose: Stops any kind of tracking and shoots dumb
//-----------------------------------------------------------------------------
void CMissile::DumbFire( void )
{
	SetThink( NULL );
	SetMoveType( MOVETYPE_FLY );
	SetTouch(&CMissile::MissileTouch);

	SetSolid(SOLID_BBOX);
	SetModel("models/weapons/w_missile.mdl");
	UTIL_SetSize( this, vec3_origin, vec3_origin );

	EmitSound( "Missile.Ignite" );
	AddFlag(FL_OBJECT);

	// Smoke trail.
	CreateSmokeTrail();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMissile::SetGracePeriod( float flGracePeriod )
{
	m_flGracePeriodEndsAt = gpGlobals->curtime + flGracePeriod;

	// Go non-solid until the grace period ends
	AddSolidFlags( FSOLID_NOT_SOLID );
}

//---------------------------------------------------------
//---------------------------------------------------------
void CMissile::AccelerateThink( void )
{
	Vector vecForward;

	// !!!UNDONE - make this work exactly the same as HL1 RPG, lest we have looping sound bugs again!
	EmitSound( "Missile.Accelerate" );

	// SetEffects( EF_LIGHT );

	AngleVectors( GetLocalAngles(), &vecForward );
	SetAbsVelocity( vecForward * RPG_SPEED );

	SetThink( &CMissile::SeekThink );
	SetNextThink( gpGlobals->curtime + 0.1f );
}

#define AUGER_YDEVIANCE 20.0f
#define AUGER_XDEVIANCEUP 8.0f
#define AUGER_XDEVIANCEDOWN 1.0f

//---------------------------------------------------------
//---------------------------------------------------------
void CMissile::AugerThink( void )
{
	// If we've augered long enough, then just explode
	if ( m_flAugerTime < gpGlobals->curtime )
	{
		Explode();
		return;
	}

	if ( m_flMarkDeadTime < gpGlobals->curtime )
	{
		m_lifeState = LIFE_DYING;
	}

	QAngle angles = GetLocalAngles();

	angles.y += random->RandomFloat( -AUGER_YDEVIANCE, AUGER_YDEVIANCE );
	angles.x += random->RandomFloat( -AUGER_XDEVIANCEDOWN, AUGER_XDEVIANCEUP );

	SetLocalAngles( angles );

	Vector vecForward;

	AngleVectors( GetLocalAngles(), &vecForward );
	
	SetAbsVelocity( vecForward * 1000.0f );

	SetNextThink( gpGlobals->curtime + 0.05f );
}

//-----------------------------------------------------------------------------
// Purpose: Causes the missile to spiral to the ground and explode, due to damage
//-----------------------------------------------------------------------------
void CMissile::ShotDown( void )
{
	CEffectData	data;
	data.m_vOrigin = GetAbsOrigin();

	DispatchEffect( "RPGShotDown", data );

	if ( m_hRocketTrail != NULL )
	{
		m_hRocketTrail->m_bDamaged = true;
	}

	SetThink( &CMissile::AugerThink );
	SetNextThink( gpGlobals->curtime );
	m_flAugerTime = gpGlobals->curtime + 1.5f;
	m_flMarkDeadTime = gpGlobals->curtime + 0.75;

	// Let the RPG start reloading immediately
	if ( m_hOwner != NULL )
	{
		m_hOwner->NotifyRocketDied();
		m_hOwner = NULL;
	}
}


//-----------------------------------------------------------------------------
// The actual explosion 
//-----------------------------------------------------------------------------
void CMissile::DoExplosion( void )
{
	// Explode
	ExplosionCreate( GetAbsOrigin(), GetAbsAngles(), GetOwnerEntity(), GetDamage(), GetDamage() * 2, 
		SF_ENVEXPLOSION_NOSPARKS | SF_ENVEXPLOSION_NODLIGHTS | SF_ENVEXPLOSION_NOSMOKE, 0.0f, this);
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMissile::Explode( void )
{
	// Don't explode against the skybox. Just pretend that 
	// the missile flies off into the distance.
	Vector forward;

	GetVectors( &forward, NULL, NULL );

	trace_t tr;
	UTIL_TraceLine( GetAbsOrigin(), GetAbsOrigin() + forward * 16, MASK_SHOT, this, COLLISION_GROUP_NONE, &tr );

	m_takedamage = DAMAGE_NO;
	SetSolid( SOLID_NONE );
	if( tr.fraction == 1.0 || !(tr.surface.flags & SURF_SKY) )
	{
		DoExplosion();
	}

	if( m_hRocketTrail )
	{
		m_hRocketTrail->SetLifetime(0.1f);
		m_hRocketTrail = NULL;
	}

	if ( m_hOwner != NULL )
	{
		m_hOwner->NotifyRocketDied();
		m_hOwner = NULL;
	}

	StopSound( "Missile.Ignite" );
	UTIL_Remove( this );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *pOther - 
//-----------------------------------------------------------------------------
void CMissile::MissileTouch( CBaseEntity *pOther )
{
	Assert( pOther );
	
	// Don't touch triggers (but DO hit weapons)
	if ( pOther->IsSolidFlagSet(FSOLID_TRIGGER|FSOLID_VOLUME_CONTENTS) && pOther->GetCollisionGroup() != COLLISION_GROUP_WEAPON )
		return;

	Explode();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMissile::CreateSmokeTrail( void )
{
	if ( m_hRocketTrail )
		return;

	// Smoke trail.
	if ( (m_hRocketTrail = RocketTrail::CreateRocketTrail()) != NULL )
	{
		m_hRocketTrail->m_Opacity = 0.2f;
		m_hRocketTrail->m_SpawnRate = 100;
		m_hRocketTrail->m_ParticleLifetime = 0.5f;
		m_hRocketTrail->m_StartColor.Init( 0.65f, 0.65f , 0.65f );
		m_hRocketTrail->m_EndColor.Init( 0.0, 0.0, 0.0 );
		m_hRocketTrail->m_StartSize = 8;
		m_hRocketTrail->m_EndSize = 32;
		m_hRocketTrail->m_SpawnRadius = 4;
		m_hRocketTrail->m_MinSpeed = 2;
		m_hRocketTrail->m_MaxSpeed = 16;
		
		m_hRocketTrail->SetLifetime( 999 );
		m_hRocketTrail->FollowEntity( this, "0" );
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMissile::IgniteThink( void )
{
	SetMoveType( MOVETYPE_FLY );
	SetModel("models/weapons/w_missile.mdl");
	UTIL_SetSize( this, vec3_origin, vec3_origin );
 	RemoveSolidFlags( FSOLID_NOT_SOLID );

	//TODO: Play opening sound

	Vector vecForward;

	EmitSound( "Missile.Ignite" );

	AngleVectors( GetLocalAngles(), &vecForward );
	SetAbsVelocity( vecForward * RPG_SPEED );

	SetThink( &CMissile::SeekThink );
	SetNextThink( gpGlobals->curtime );

	if ( m_hOwner && m_hOwner->GetOwner() )
	{
		CBasePlayer *pPlayer = ToBasePlayer( m_hOwner->GetOwner() );

		color32 white = { 255,225,205,64 };
		UTIL_ScreenFade( pPlayer, white, 0.1f, 0.0f, FFADE_IN );
	}

	CreateSmokeTrail();
}


//-----------------------------------------------------------------------------
// Gets the shooting position 
//-----------------------------------------------------------------------------
void CMissile::GetShootPosition( CLaserDot *pLaserDot, Vector *pShootPosition )
{
	if ( pLaserDot->GetOwnerEntity() != NULL )
	{
		//FIXME: Do we care this isn't exactly the muzzle position?
		*pShootPosition = pLaserDot->GetOwnerEntity()->WorldSpaceCenter();
	}
	else
	{
		*pShootPosition = pLaserDot->GetChasePosition();
	}
}

	
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
#define	RPG_HOMING_SPEED	0.125f

void CMissile::ComputeActualDotPosition( CLaserDot *pLaserDot, Vector *pActualDotPosition, float *pHomingSpeed )
{
	*pHomingSpeed = RPG_HOMING_SPEED;
	if ( pLaserDot->GetTargetEntity() )
	{
		*pActualDotPosition = pLaserDot->GetChasePosition();
		return;
	}

	Vector vLaserStart;
	GetShootPosition( pLaserDot, &vLaserStart );

	//Get the laser's vector
	Vector vLaserDir;
	VectorSubtract( pLaserDot->GetChasePosition(), vLaserStart, vLaserDir );
	
	//Find the length of the current laser
	float flLaserLength = VectorNormalize( vLaserDir );
	
	//Find the length from the missile to the laser's owner
	float flMissileLength = GetAbsOrigin().DistTo( vLaserStart );

	//Find the length from the missile to the laser's position
	Vector vecTargetToMissile;
	VectorSubtract( GetAbsOrigin(), pLaserDot->GetChasePosition(), vecTargetToMissile ); 
	float flTargetLength = VectorNormalize( vecTargetToMissile );

	// See if we should chase the line segment nearest us
	if ( ( flMissileLength < flLaserLength ) || ( flTargetLength <= 512.0f ) )
	{
		*pActualDotPosition = UTIL_PointOnLineNearestPoint( vLaserStart, pLaserDot->GetChasePosition(), GetAbsOrigin() );
		*pActualDotPosition += ( vLaserDir * 256.0f );
	}
	else
	{
		// Otherwise chase the dot
		*pActualDotPosition = pLaserDot->GetChasePosition();
	}

//	NDebugOverlay::Line( pLaserDot->GetChasePosition(), vLaserStart, 0, 255, 0, true, 0.05f );
//	NDebugOverlay::Line( GetAbsOrigin(), *pActualDotPosition, 255, 0, 0, true, 0.05f );
//	NDebugOverlay::Cross3D( *pActualDotPosition, -Vector(4,4,4), Vector(4,4,4), 255, 0, 0, true, 0.05f );
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CMissile::SeekThink( void )
{
	CBaseEntity	*pBestDot	= NULL;
	float		flBestDist	= MAX_TRACE_LENGTH;
	float		dotDist;

	// If we have a grace period, go solid when it ends
	if ( m_flGracePeriodEndsAt )
	{
		if ( m_flGracePeriodEndsAt < gpGlobals->curtime )
		{
			RemoveSolidFlags( FSOLID_NOT_SOLID );
			m_flGracePeriodEndsAt = 0;
		}
	}

	//Search for all dots relevant to us
	for( CLaserDot *pEnt = GetLaserDotList(); pEnt != NULL; pEnt = pEnt->m_pNext )
	{
		if ( !pEnt->IsOn() )
			continue;

		if ( pEnt->GetOwnerEntity() != GetOwnerEntity() )
			continue;

		dotDist = (GetAbsOrigin() - pEnt->GetAbsOrigin()).Length();

		//Find closest
		if ( dotDist < flBestDist )
		{
			pBestDot	= pEnt;
			flBestDist	= dotDist;
		}
	}

	//If we have a dot target
	if ( pBestDot == NULL )
	{
		//Think as soon as possible
		SetNextThink( gpGlobals->curtime );
		return;
	}

	CLaserDot *pLaserDot = (CLaserDot *)pBestDot;
	Vector	targetPos;

	float flHomingSpeed; 
	Vector vecLaserDotPosition;
	ComputeActualDotPosition( pLaserDot, &targetPos, &flHomingSpeed );

	if ( IsSimulatingOnAlternateTicks() )
		flHomingSpeed *= 2;

	Vector	vTargetDir;
	VectorSubtract( targetPos, GetAbsOrigin(), vTargetDir );
	float flDist = VectorNormalize( vTargetDir );

	Vector	vDir	= GetAbsVelocity();
	float	flSpeed	= VectorNormalize( vDir );
	Vector	vNewVelocity = vDir;
	if ( gpGlobals->frametime > 0.0f )
	{
		if ( flSpeed != 0 )
		{
			vNewVelocity = ( flHomingSpeed * vTargetDir ) + ( ( 1 - flHomingSpeed ) * vDir );

			// This computation may happen to cancel itself out exactly. If so, slam to targetdir.
			if ( VectorNormalize( vNewVelocity ) < 1e-3 )
			{
				vNewVelocity = (flDist != 0) ? vTargetDir : vDir;
			}
		}
		else
		{
			vNewVelocity = vTargetDir;
		}
	}

	QAngle	finalAngles;
	VectorAngles( vNewVelocity, finalAngles );
	SetAbsAngles( finalAngles );

	vNewVelocity *= flSpeed;
	SetAbsVelocity( vNewVelocity );

	if( GetAbsVelocity() == vec3_origin )
	{
		// Strange circumstances have brought this missile to halt. Just blow it up.
		Explode();
		return;
	}

	// Think as soon as possible
	SetNextThink( gpGlobals->curtime );
}


//-----------------------------------------------------------------------------
// Purpose: 
//
// Input  : &vecOrigin - 
//			&vecAngles - 
//			NULL - 
//
// Output : CMissile
//-----------------------------------------------------------------------------
CMissile *CMissile::Create( const Vector &vecOrigin, const QAngle &vecAngles, edict_t *pentOwner = NULL )
{
	//CMissile *pMissile = (CMissile *)CreateEntityByName("rpg_missile" );
	CMissile *pMissile = (CMissile *) CBaseEntity::Create( "rpg_missile", vecOrigin, vecAngles, CBaseEntity::Instance( pentOwner ) );
	pMissile->SetOwnerEntity( Instance( pentOwner ) );
	pMissile->Spawn();
	pMissile->AddEffects( EF_NOSHADOW );
	
	Vector vecForward;
	AngleVectors( vecAngles, &vecForward );

	pMissile->SetAbsVelocity( vecForward * 300 + Vector( 0,0, 128 ) );

	return pMissile;
}

#endif

#define	RPG_BEAM_SPRITE		"effects/laser1.vmt"
#define	RPG_BEAM_SPRITE_NOZ	"effects/laser1_noz.vmt"
#define	RPG_LASER_SPRITE	"sprites/redglow1"

//=============================================================================
// RPG
//=============================================================================

LINK_ENTITY_TO_CLASS( weapon_rpg, CWeaponRPG );
PRECACHE_WEAPON_REGISTER(weapon_rpg);

IMPLEMENT_NETWORKCLASS_ALIASED( WeaponRPG, DT_WeaponRPG )

#ifdef CLIENT_DLL
void RecvProxy_MissileDied( const CRecvProxyData *pData, void *pStruct, void *pOut )
{
	CWeaponRPG *pRPG = ((CWeaponRPG*)pStruct);

	RecvProxy_IntToEHandle( pData, pStruct, pOut );

	CBaseEntity *pNewMissile = pRPG->GetMissile();

	if ( pNewMissile == NULL )
	{
		if ( pRPG->GetOwner() && pRPG->GetOwner()->GetActiveWeapon() == pRPG )
		{
			pRPG->NotifyRocketDied();
		}
	}
}

#endif

BEGIN_NETWORK_TABLE( CWeaponRPG, DT_WeaponRPG )
#ifdef CLIENT_DLL
	RecvPropBool( RECVINFO( m_bInitialStateUpdate ) ),
	RecvPropBool( RECVINFO( m_bGuiding ) ),
	RecvPropBool( RECVINFO( m_bHideGuiding ) ),
	RecvPropEHandle( RECVINFO( m_hMissile ), RecvProxy_MissileDied ),
	RecvPropVector( RECVINFO( m_vecLaserDot ) ),
#else
	SendPropBool( SENDINFO( m_bInitialStateUpdate ) ),
	SendPropBool( SENDINFO( m_bGuiding ) ),
	SendPropBool( SENDINFO( m_bHideGuiding ) ),
	SendPropEHandle( SENDINFO( m_hMissile ) ),
#endif
END_NETWORK_TABLE()

#ifdef CLIENT_DLL

BEGIN_PREDICTION_DATA( CWeaponRPG )
	DEFINE_PRED_FIELD( m_bInitialStateUpdate, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bGuiding, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_bHideGuiding, FIELD_BOOLEAN, FTYPEDESC_INSENDTABLE ),
END_PREDICTION_DATA()

#endif

#ifndef CLIENT_DLL
acttable_t	CWeaponRPG::m_acttable[] = 
{
	{ ACT_HL2MP_IDLE,					ACT_HL2MP_IDLE_RPG,					false },
	{ ACT_HL2MP_RUN,					ACT_HL2MP_RUN_RPG,					false },
	{ ACT_HL2MP_IDLE_CROUCH,			ACT_HL2MP_IDLE_CROUCH_RPG,			false },
	{ ACT_HL2MP_WALK_CROUCH,			ACT_HL2MP_WALK_CROUCH_RPG,			false },
	{ ACT_HL2MP_GESTURE_RANGE_ATTACK,	ACT_HL2MP_GESTURE_RANGE_ATTACK_RPG,	false },
	{ ACT_HL2MP_GESTURE_RELOAD,			ACT_HL2MP_GESTURE_RELOAD_RPG,		false },
	{ ACT_HL2MP_JUMP,					ACT_HL2MP_JUMP_RPG,					false },
	{ ACT_RANGE_ATTACK1,				ACT_RANGE_ATTACK_RPG,				false },
};

IMPLEMENT_ACTTABLE(CWeaponRPG);

#endif

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CWeaponRPG::CWeaponRPG()
{
	m_bReloadsSingly = true;
	m_bInitialStateUpdate= false;
	m_bHideGuiding = false;
	m_bGuiding = false;

	m_fMinRange1 = m_fMinRange2 = 40*12;
	m_fMaxRange1 = m_fMaxRange2 = 500*12;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CWeaponRPG::~CWeaponRPG()
{
#ifndef CLIENT_DLL
	if ( m_hLaserDot != NULL )
	{
		UTIL_Remove( m_hLaserDot );
		m_hLaserDot = NULL;
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::Precache( void )
{
	BaseClass::Precache();

	PrecacheScriptSound( "Missile.Ignite" );
	PrecacheScriptSound( "Missile.Accelerate" );

	// Laser dot...
	PrecacheModel( "sprites/redglow1.vmt" );
	PrecacheModel( RPG_LASER_SPRITE );
	PrecacheModel( RPG_BEAM_SPRITE );
	PrecacheModel( RPG_BEAM_SPRITE_NOZ );

#ifndef CLIENT_DLL
	UTIL_PrecacheOther( "rpg_missile" );
#endif

}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::Activate( void )
{
	BaseClass::Activate();

	// Restore the laser pointer after transition
	if ( m_bGuiding )
	{
		CBasePlayer *pOwner = ToBasePlayer( GetOwner() );
		
		if ( pOwner == NULL )
			return;

		if ( pOwner->GetActiveWeapon() == this )
		{
			StartGuiding();
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CWeaponRPG::HasAnyAmmo( void )
{
	if ( m_hMissile != NULL )
		return true;

	return BaseClass::HasAnyAmmo();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::PrimaryAttack( void )
{
	if (UsesClipsForAmmo1() && !m_iClip1)
	{
		Reload();
		return;
	}
	// Only the player fires this way so we can cast
	CSDKPlayer *pPlayer = GetPlayerOwner();

	if (!pPlayer)
		return;

	// Can't have an active missile out
	/*if ( m_hMissile != NULL )
		return;*/

	// Can't be reloading
	if (GetActivity() == ACT_VM_RELOAD_DEPLOYED)
		return;

	Vector vecOrigin;
	Vector vecForward;

	m_flNextPrimaryAttack = gpGlobals->curtime + 0.5f;

	CSDKPlayer *pOwner = GetPlayerOwner();
	
	if ( pOwner == NULL )
		return;

	Vector	vForward, vRight, vUp;

	pOwner->EyeVectors( &vForward, &vRight, &vUp );

	Vector	muzzlePoint = pOwner->Weapon_ShootPosition() + vForward * 12.0f + vRight * 6.0f + vUp * -3.0f;

#ifndef CLIENT_DLL
	QAngle vecAngles;
	VectorAngles( vForward, vecAngles );

	CMissile *pMissile = CMissile::Create( muzzlePoint, vecAngles, GetOwner()->edict() );
	pMissile->m_hOwner = this;

	// If the shot is clear to the player, give the missile a grace period
	trace_t	tr;
	Vector vecEye = pOwner->EyePosition();
	UTIL_TraceLine( vecEye, vecEye + vForward * 128, MASK_SHOT, this, COLLISION_GROUP_NONE, &tr );
	if ( tr.fraction == 1.0 )
	{
		pMissile->SetGracePeriod( 0.3 );
	}

	pMissile->SetDamage(GetSDKWpnData().m_iDamage);

	//m_hMissile = pMissile;
#endif

	DecrementAmmo( GetOwner() );
	SendWeaponAnim( ACT_VM_PRIMARYATTACK );
	WeaponSound( SINGLE );

	// player "shoot" animation
	pPlayer->SetAnimation( PLAYER_ATTACK1 );
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *pOwner - 
//-----------------------------------------------------------------------------
void CWeaponRPG::DecrementAmmo( CBaseCombatCharacter *pOwner )
{
	
	// Take away our primary ammo type
	if (UsesClipsForAmmo1())
	{
		pOwner->RemoveAmmo(1, m_iClip1);
	}
	else
	{
		pOwner->RemoveAmmo(1, m_iPrimaryAmmoType);
	}
	
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : state - 
//-----------------------------------------------------------------------------
void CWeaponRPG::SuppressGuiding( bool state )
{
	m_bHideGuiding = state;

#ifndef CLIENT_DLL

	if ( m_hLaserDot == NULL )
	{
		StartGuiding();

		//STILL!?
		if ( m_hLaserDot == NULL )
			 return;
	}

	if ( state )
	{
		m_hLaserDot->TurnOff();
	}
	else
	{
		m_hLaserDot->TurnOff();
	}
#endif
	
}

//-----------------------------------------------------------------------------
// Purpose: Override this if we're guiding a missile currently
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CWeaponRPG::Lower( void )
{
	if ( m_hMissile != NULL )
		return false;

	return BaseClass::Lower();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::ItemPostFrame( void )
{
	BaseClass::ItemPostFrame();

	CBasePlayer *pPlayer = ToBasePlayer( GetOwner() );
	
	if ( pPlayer == NULL )
		return;

	//If we're pulling the weapon out for the first time, wait to draw the laser
	if ( ( m_bInitialStateUpdate ) && ( GetActivity() != ACT_VM_DRAW ) )
	{
		StartGuiding();
		m_bInitialStateUpdate = false;
	}

	// Supress our guiding effects if we're lowered
	if ( GetIdealActivity() == ACT_VM_IDLE_LOWERED )
	{
		SuppressGuiding();
	}
	else
	{
		SuppressGuiding();
	}

	//Move the laser
	UpdateLaserPosition();
	StopGuiding();

	if ( pPlayer->GetAmmoCount(m_iPrimaryAmmoType) <= 0 && m_hMissile == NULL )
	{
		StopGuiding();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Vector
//-----------------------------------------------------------------------------
Vector CWeaponRPG::GetLaserPosition( void )
{
#ifndef CLIENT_DLL
	CreateLaserPointer();

	if ( m_hLaserDot != NULL )
		return m_hLaserDot->GetAbsOrigin();

	//FIXME: The laser dot sprite is not active, this code should not be allowed!
	assert(0);
#endif
	return vec3_origin;
}

//-----------------------------------------------------------------------------
// Purpose: NPC RPG users cheat and directly set the laser pointer's origin
// Input  : &vecTarget - 
//-----------------------------------------------------------------------------
void CWeaponRPG::UpdateNPCLaserPosition( const Vector &vecTarget )
{

}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::SetNPCLaserPosition( const Vector &vecTarget ) 
{ 
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
const Vector &CWeaponRPG::GetNPCLaserPosition( void )
{
	return vec3_origin;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true if the rocket is being guided, false if it's dumb
//-----------------------------------------------------------------------------
bool CWeaponRPG::IsGuiding( void )
{
	return m_bGuiding;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool CWeaponRPG::Deploy( void )
{
	m_bInitialStateUpdate = true;

	return BaseClass::Deploy();
}

bool CWeaponRPG::CanHolster( void )
{
	//Can't have an active missile out
	if ( m_hMissile != NULL )
		return false;

	return BaseClass::CanHolster();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CWeaponRPG::Holster( CBaseCombatWeapon *pSwitchingTo )
{
	StopGuiding();

	return BaseClass::Holster( pSwitchingTo );
}

//-----------------------------------------------------------------------------
// Purpose: Turn on the guiding laser
//-----------------------------------------------------------------------------
void CWeaponRPG::StartGuiding( void )
{
	// Don't start back up if we're overriding this
	if ( m_bHideGuiding )
		return;

	m_bGuiding = true;

#ifndef CLIENT_DLL
	WeaponSound(SPECIAL1);

	CreateLaserPointer();
#endif

}

//-----------------------------------------------------------------------------
// Purpose: Turn off the guiding laser
//-----------------------------------------------------------------------------
void CWeaponRPG::StopGuiding( void )
{
	m_bGuiding = false;

#ifndef CLIENT_DLL

	WeaponSound( SPECIAL2 );

	// Kill the dot completely
	if ( m_hLaserDot != NULL )
	{
		m_hLaserDot->TurnOff();
		UTIL_Remove( m_hLaserDot );
		m_hLaserDot = NULL;
	}
#else
	if ( m_pBeam )
	{
		//Tell it to die right away and let the beam code free it.
		m_pBeam->brightness = 0.0f;
		m_pBeam->flags &= ~FBEAM_FOREVER;
		m_pBeam->die = gpGlobals->curtime - 0.1;
		m_pBeam = NULL;
	}
#endif

}

//-----------------------------------------------------------------------------
// Purpose: Toggle the guiding laser
//-----------------------------------------------------------------------------
void CWeaponRPG::ToggleGuiding( void )
{
	if ( IsGuiding() )
	{
		StopGuiding();
	}
	else
	{
		StartGuiding();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::Drop( const Vector &vecVelocity )
{
	StopGuiding();

	BaseClass::Drop( vecVelocity );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::UpdateLaserPosition( Vector vecMuzzlePos, Vector vecEndPos )
{

#ifndef CLIENT_DLL
	if ( vecMuzzlePos == vec3_origin || vecEndPos == vec3_origin )
	{
		CBasePlayer *pPlayer = ToBasePlayer( GetOwner() );
		if ( !pPlayer )
			return;

		vecMuzzlePos = pPlayer->Weapon_ShootPosition();
		Vector	forward;
		pPlayer->EyeVectors( &forward );
		vecEndPos = vecMuzzlePos + ( forward * MAX_TRACE_LENGTH );
	}

	//Move the laser dot, if active
	trace_t	tr;
	
	// Trace out for the endpoint
	UTIL_TraceLine( vecMuzzlePos, vecEndPos, (MASK_SHOT & ~CONTENTS_WINDOW), GetOwner(), COLLISION_GROUP_NONE, &tr );

	// Move the laser sprite
	if ( m_hLaserDot != NULL )
	{
		Vector	laserPos = tr.endpos;
		m_hLaserDot->SetLaserPosition( laserPos, tr.plane.normal );
				
		if ( tr.DidHitNonWorldEntity() )
		{
			CBaseEntity *pHit = tr.m_pEnt;

			if ( ( pHit != NULL ) && ( pHit->m_takedamage ) )
			{
				m_hLaserDot->SetTargetEntity( pHit );
			}
			else
			{
				m_hLaserDot->SetTargetEntity( NULL );
			}
		}
		else
		{
			m_hLaserDot->SetTargetEntity( NULL );
		}
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CWeaponRPG::CreateLaserPointer( void )
{
#ifndef CLIENT_DLL
	if ( m_hLaserDot != NULL )
		return;

	CBaseCombatCharacter *pOwner = GetOwner();
	
	if ( pOwner == NULL )
		return;

	if ( pOwner->GetAmmoCount(m_iPrimaryAmmoType) <= 0 )
		return;

	m_hLaserDot = CLaserDot::Create( GetAbsOrigin(), GetOwner() );
	m_hLaserDot->TurnOff();

	UpdateLaserPosition();
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CWeaponRPG::NotifyRocketDied( void )
{
	m_hMissile = NULL;

	if (GetActivity() == ACT_VM_RELOAD_DEPLOYED)
		return;

	Reload();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CWeaponRPG::Reload( void )
{
	CBaseCombatCharacter *pOwner = GetOwner();

	if (pOwner == NULL)
		return false;

	if (pOwner->GetAmmoCount(m_iPrimaryAmmoType) <= 0)
		return false;

	if (m_iClip1 >= GetMaxClip1())
		return false;

	int j = MIN(1, pOwner->GetAmmoCount(m_iPrimaryAmmoType));

	if (j <= 0)
		return false;

	FillClip();
	// Play reload on different channel as otherwise steals channel away from fire sound
	WeaponSound(RELOAD);
	SendWeaponAnim(ACT_VM_RELOAD_DEPLOYED);

	return true;

}

void CWeaponRPG::FillClip(void)
{
	CBaseCombatCharacter *pOwner = GetOwner();

	if (pOwner == NULL)
		return;

	// Add them to the clip
	if (pOwner->GetAmmoCount(m_iPrimaryAmmoType) > 0)
	{
		if (Clip1() < GetMaxClip1())
		{
			m_iClip1++;
			pOwner->RemoveAmmo(1, m_iPrimaryAmmoType);
		}
	}
}

#ifdef CLIENT_DLL

#define	RPG_MUZZLE_ATTACHMENT		1
#define	RPG_GUIDE_ATTACHMENT		2
#define	RPG_GUIDE_TARGET_ATTACHMENT	3

#define	RPG_GUIDE_ATTACHMENT_3RD		4
#define	RPG_GUIDE_TARGET_ATTACHMENT_3RD	5

#define	RPG_LASER_BEAM_LENGTH	128

extern void FormatViewModelAttachment( Vector &vOrigin, bool bInverse );

//-----------------------------------------------------------------------------
// Purpose: Returns the attachment point on either the world or viewmodel
//			This should really be worked into the CBaseCombatWeapon class!
//-----------------------------------------------------------------------------
void CWeaponRPG::GetWeaponAttachment( int attachmentId, Vector &outVector, Vector *dir /*= NULL*/ )
{
	QAngle	angles;

	if ( ShouldDrawUsingViewModel() )
	{
		CBasePlayer *pOwner = ToBasePlayer( GetOwner() );
		
		if ( pOwner != NULL )
		{
			pOwner->GetViewModel()->GetAttachment( attachmentId, outVector, angles );
			::FormatViewModelAttachment( outVector, true );
		}
	}
	else
	{
		// We offset the IDs to make them correct for our world model
		BaseClass::GetAttachment( attachmentId, outVector, angles );
	}

	// Supply the direction, if requested
	if ( dir != NULL )
	{
		AngleVectors( angles, dir, NULL, NULL );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Setup our laser beam
//-----------------------------------------------------------------------------
void CWeaponRPG::InitBeam( void )
{
	if ( m_pBeam != NULL )
		return;

	CBaseCombatCharacter *pOwner = GetOwner();
	
	if ( pOwner == NULL )
		return;

	if ( pOwner->GetAmmoCount(m_iPrimaryAmmoType) <= 0 )
		return;


	BeamInfo_t beamInfo;

	CBaseEntity *pEntity = NULL;

	if ( ShouldDrawUsingViewModel() )
	{
		CBasePlayer *pOwner = ToBasePlayer( GetOwner() );
		
		if ( pOwner != NULL )
		{
			pEntity = pOwner->GetViewModel();
		}
	}
	else
	{
		pEntity = this;
	}

	beamInfo.m_pStartEnt = pEntity;
	beamInfo.m_pEndEnt = pEntity;
	beamInfo.m_nType = TE_BEAMPOINTS;
	beamInfo.m_vecStart = vec3_origin;
	beamInfo.m_vecEnd = vec3_origin;
	
	beamInfo.m_pszModelName = ( ShouldDrawUsingViewModel() ) ? RPG_BEAM_SPRITE_NOZ : RPG_BEAM_SPRITE;
	
	beamInfo.m_flHaloScale = 0.0f;
	beamInfo.m_flLife = 0.0f;
	
	if ( ShouldDrawUsingViewModel() )
	{
		beamInfo.m_flWidth = 2.0f;
		beamInfo.m_flEndWidth = 2.0f;
		beamInfo.m_nStartAttachment = RPG_GUIDE_ATTACHMENT;
		beamInfo.m_nEndAttachment = RPG_GUIDE_TARGET_ATTACHMENT;
	}
	else
	{
		beamInfo.m_flWidth = 1.0f;
		beamInfo.m_flEndWidth = 1.0f;
		beamInfo.m_nStartAttachment = RPG_GUIDE_ATTACHMENT_3RD;
		beamInfo.m_nEndAttachment = RPG_GUIDE_TARGET_ATTACHMENT_3RD;
	}

	beamInfo.m_flFadeLength = 0.0f;
	beamInfo.m_flAmplitude = 0;
	beamInfo.m_flBrightness = 255.0;
	beamInfo.m_flSpeed = 1.0f;
	beamInfo.m_nStartFrame = 0.0;
	beamInfo.m_flFrameRate = 30.0;
	beamInfo.m_flRed = 255.0;
	beamInfo.m_flGreen = 0.0;
	beamInfo.m_flBlue = 0.0;
	beamInfo.m_nSegments = 4;
	beamInfo.m_bRenderable = true;
	beamInfo.m_nFlags = (FBEAM_FOREVER|FBEAM_SHADEOUT);

	m_pBeam = beams->CreateBeamEntPoint( beamInfo );
}

//-----------------------------------------------------------------------------
// Purpose: Draw effects for our weapon
//-----------------------------------------------------------------------------
void CWeaponRPG::DrawEffects( void )
{
	// Must be guiding and not hidden
	if ( !m_bGuiding || m_bHideGuiding )
	{
		if ( m_pBeam != NULL )
		{
			m_pBeam->brightness = 0;
		}

		return;
	}

	// Setup our sprite
	if ( m_hSpriteMaterial == NULL )
	{
		m_hSpriteMaterial.Init( RPG_LASER_SPRITE, TEXTURE_GROUP_CLIENT_EFFECTS );
	}

	// Setup our beam
	if ( m_hBeamMaterial == NULL )
	{
		m_hBeamMaterial.Init( RPG_BEAM_SPRITE, TEXTURE_GROUP_CLIENT_EFFECTS );
	}

	color32 color={255,255,255,255};
	Vector	vecAttachment, vecDir;
	QAngle	angles;

	float scale = 8.0f + random->RandomFloat( -2.0f, 2.0f );

	int	attachmentID = ( ShouldDrawUsingViewModel() ) ? RPG_GUIDE_ATTACHMENT : RPG_GUIDE_ATTACHMENT_3RD;

	GetWeaponAttachment( attachmentID, vecAttachment, &vecDir );

	// Draw the sprite
	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->Bind( m_hSpriteMaterial, this );
	DrawSprite( vecAttachment, scale, scale, color );
	
	// Get the beam's run
	trace_t tr;
	UTIL_TraceLine( vecAttachment, vecAttachment + ( vecDir * RPG_LASER_BEAM_LENGTH ), MASK_SHOT, GetOwner(), COLLISION_GROUP_NONE, &tr );
	
	InitBeam();

	if ( m_pBeam != NULL )
	{
		m_pBeam->fadeLength = RPG_LASER_BEAM_LENGTH * tr.fraction;
		m_pBeam->brightness = random->RandomInt( 128, 200 );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Called on third-person weapon drawing
//-----------------------------------------------------------------------------
int	CWeaponRPG::DrawModel( int flags )
{
	// Only render these on the transparent pass
	if ( flags & STUDIO_TRANSPARENCY )
	{
		DrawEffects();
		return 1;
	}

	// Draw the model as normal
	return BaseClass::DrawModel( flags );
}

//-----------------------------------------------------------------------------
// Purpose: Called after first-person viewmodel is drawn
//-----------------------------------------------------------------------------
void CWeaponRPG::ViewModelDrawn( C_BaseViewModel *pBaseViewModel )
{
	// Draw our laser effects
	DrawEffects();
	
	BaseClass::ViewModelDrawn( pBaseViewModel );
}

//-----------------------------------------------------------------------------
// Purpose: Used to determine sorting of model when drawn
//-----------------------------------------------------------------------------
bool CWeaponRPG::IsTranslucent( void )
{
	// Must be guiding and not hidden
	if ( m_bGuiding && !m_bHideGuiding )
		return true;

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Turns off effects when leaving the PVS
//-----------------------------------------------------------------------------
void CWeaponRPG::NotifyShouldTransmit( ShouldTransmitState_t state )
{
	BaseClass::NotifyShouldTransmit(state);

	if ( state == SHOULDTRANSMIT_END )
	{
		if ( m_pBeam != NULL )
		{
			m_pBeam->brightness = 0.0f;
		}
	}
}

#endif	//CLIENT_DLL


//=============================================================================
// Laser Dot
//=============================================================================

LINK_ENTITY_TO_CLASS( env_laserdot, CLaserDot );

BEGIN_DATADESC( CLaserDot )
	DEFINE_FIELD( m_vecSurfaceNormal,	FIELD_VECTOR ),
	DEFINE_FIELD( m_hTargetEnt,			FIELD_EHANDLE ),
	DEFINE_FIELD( m_bVisibleLaserDot,	FIELD_BOOLEAN ),
	DEFINE_FIELD( m_bIsOn,				FIELD_BOOLEAN ),

	//DEFINE_FIELD( m_pNext, FIELD_CLASSPTR ),	// don't save - regenerated by constructor
END_DATADESC()


//-----------------------------------------------------------------------------
// Finds missiles in cone
//-----------------------------------------------------------------------------
CBaseEntity *CreateLaserDot( const Vector &origin, CBaseEntity *pOwner, bool bVisibleDot )
{
	return CLaserDot::Create( origin, pOwner, bVisibleDot );
}

void SetLaserDotTarget( CBaseEntity *pLaserDot, CBaseEntity *pTarget )
{
	CLaserDot *pDot = assert_cast< CLaserDot* >(pLaserDot );
	pDot->SetTargetEntity( pTarget );
}

void EnableLaserDot( CBaseEntity *pLaserDot, bool bEnable )
{
	CLaserDot *pDot = assert_cast< CLaserDot* >(pLaserDot );
	if ( bEnable )
	{
		pDot->TurnOn();
	}
	else
	{
		pDot->TurnOff();
	}
}

CLaserDot::CLaserDot( void )
{
	m_hTargetEnt = NULL;
	m_bIsOn = true;
#ifndef CLIENT_DLL
	g_LaserDotList.Insert( this );
#endif
}

CLaserDot::~CLaserDot( void )
{
#ifndef CLIENT_DLL
	g_LaserDotList.Remove( this );
#endif
}


//-----------------------------------------------------------------------------
// Purpose: 
// Input  : &origin - 
// Output : CLaserDot
//-----------------------------------------------------------------------------
CLaserDot *CLaserDot::Create( const Vector &origin, CBaseEntity *pOwner, bool bVisibleDot )
{
#ifndef CLIENT_DLL
	CLaserDot *pLaserDot = (CLaserDot *) CBaseEntity::Create( "env_laserdot", origin, QAngle(0,0,0) );

	if ( pLaserDot == NULL )
		return NULL;

	pLaserDot->m_bVisibleLaserDot = bVisibleDot;
	pLaserDot->SetMoveType( MOVETYPE_NONE );
	pLaserDot->AddSolidFlags( FSOLID_NOT_SOLID );
	pLaserDot->AddEffects( EF_NOSHADOW );
	UTIL_SetSize( pLaserDot, -Vector(4,4,4), Vector(4,4,4) );

	pLaserDot->SetOwnerEntity( pOwner );

	pLaserDot->AddEFlags( EFL_FORCE_CHECK_TRANSMIT );

	if ( !bVisibleDot )
	{
		pLaserDot->MakeInvisible();
	}

	return pLaserDot;
#else
	return NULL;
#endif
}

void CLaserDot::SetLaserPosition( const Vector &origin, const Vector &normal )
{
	SetAbsOrigin( origin );
	m_vecSurfaceNormal = normal;
}

Vector CLaserDot::GetChasePosition()
{
	return GetAbsOrigin() - m_vecSurfaceNormal * 10;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CLaserDot::TurnOn( void )
{
	m_bIsOn = true;
	if ( m_bVisibleLaserDot )
	{
		//BaseClass::TurnOn();
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CLaserDot::TurnOff( void )
{
	m_bIsOn = false;
	if ( m_bVisibleLaserDot )
	{
		//BaseClass::TurnOff();
	}
}


//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CLaserDot::MakeInvisible( void )
{
}

#ifdef CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose: Draw our sprite
//-----------------------------------------------------------------------------
int CLaserDot::DrawModel( int flags )
{
	color32 color={255,255,255,255};
	Vector	vecAttachment, vecDir;
	QAngle	angles;

	float	scale;
	Vector	endPos;

	C_SDKPlayer *pOwner = ToSDKPlayer( GetOwnerEntity() );

	if ( pOwner != NULL && pOwner->IsDormant() == false )
	{
		// Always draw the dot in front of our faces when in first-person
		if ( pOwner->IsLocalPlayer() )
		{
			// Take our view position and orientation
			vecAttachment = CurrentViewOrigin();
			vecDir = CurrentViewForward();
		}
		else
		{
			// Take the eye position and direction
			vecAttachment = pOwner->EyePosition();
			
			QAngle angles = pOwner->EyeAngles();
			AngleVectors( angles, &vecDir );
		}
		
		trace_t tr;
		UTIL_TraceLine( vecAttachment, vecAttachment + ( vecDir * MAX_TRACE_LENGTH ), MASK_SHOT, pOwner, COLLISION_GROUP_NONE, &tr );
		
		// Backup off the hit plane
		endPos = tr.endpos + ( tr.plane.normal * 4.0f );
	}
	else
	{
		// Just use our position if we can't predict it otherwise
		endPos = GetAbsOrigin();
	}

	// Randomly flutter
	scale = 16.0f + random->RandomFloat( -4.0f, 4.0f );

	// Draw our laser dot in space
	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->Bind( m_hSpriteMaterial, this );
	DrawSprite( endPos, scale, scale, color );

	return 1;
}

//-----------------------------------------------------------------------------
// Purpose: Setup our sprite reference
//-----------------------------------------------------------------------------
void CLaserDot::OnDataChanged( DataUpdateType_t updateType )
{
	if ( updateType == DATA_UPDATE_CREATED )
	{
		m_hSpriteMaterial.Init( RPG_LASER_SPRITE, TEXTURE_GROUP_CLIENT_EFFECTS );
	}
}

#endif	//CLIENT_DLL
