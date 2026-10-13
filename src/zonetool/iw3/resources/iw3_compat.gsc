// mw120r IW3 compatibility layer for Replay 1.20 (IW8 GSC, compiled with
// gsc-tool -g iw8). The IW3 map script translator appends this to every
// converted map script and routes IW3 common-script calls here:
//   maps\mp\_load::main()        -> iw3compat_load_main()
//   maps\mp\_compass::*          -> handled by iw3compat_minimap() (generated)
//   maps\mp\_utility::exploder() -> iw3compat_exploder()
// Builtin ids, argument orders and field tokens were checked against the
// Replay 1.20 builtin tables and handlers. Effects come from common MP zones
// (loadfx only accepts fx/ or vfx/ names). The converter renames IW3-only
// targetnames (destructible, interactive_tv, exploder) to mw120r_* so stock
// Replay scripts never adopt these entities.

iw3compat_noop()
{
}

// Models the converter included and precached (iw3compat_generated()).
iw3compat_known_model( name )
{
    if ( isdefined( level.mw120r_models ) && isdefined( level.mw120r_models[name] ) )
        return name;
    return undefined;
}

iw3compat_load_main()
{
    if ( isdefined( level.mw120r_iw3_loaded ) )
        return;

    level.mw120r_iw3_loaded = 1;
    iw3compat_precache();
    iw3compat_remove_gameobjects();
    iw3compat_exploders_init();
    level thread iw3compat_destructible_vehicles();
    level thread iw3compat_destructables();
    level thread iw3compat_interactive_tvs();
}

iw3compat_precache()
{
    level.mw120r_fx = [];
    level.mw120r_fx["veh_smoke_light"] = loadfx( "vfx/iw8/veh/scriptables/vfx_veh_hood_smoke" );
    level.mw120r_fx["veh_smoke_heavy"] = loadfx( "vfx/iw8/veh/scriptables/vfx_smoke_heavy_ch" );
    level.mw120r_fx["veh_fire"] = loadfx( "vfx/iw8/veh/scriptables/vfx_veh_fire_engine_flareup" );
    level.mw120r_fx["veh_fire_linger"] = loadfx( "vfx/iw8/veh/scriptables/vfx_veh_fire_linger_med" );
    level.mw120r_fx["veh_explosion"] = loadfx( "vfx/iw8/veh/scriptables/vfx_veh_explosion_civ" );
    level.mw120r_fx["veh_glass"] = loadfx( "vfx/iw8/veh/scriptables/vfx_veh_glass_windshield" );
    level.mw120r_fx["glass_shatter"] = loadfx( "vfx/code/glass/glass_shatter_32x32" );
    level.mw120r_fx["sparks"] = loadfx( "vfx/iw8_mp/equipment/flashbang/vfx_spark_crack_npop" );
}

// IW3 deleted every game object its gametype did not use. Replay's own
// gametypes never use the IW3 ones, so drop their props in TDM/FFA.
iw3compat_remove_gameobjects()
{
    gametype = tolower( getdvar( "g_gametype" ) );
    if ( gametype != "war" && gametype != "dm" && gametype != "tdm" )
        return;

    models = getentarray( "script_model", "classname" );
    foreach ( model in models )
    {
        if ( isdefined( model.script_gameobjectname ) )
            model delete();
    }
}

// ---------------------------------------------------------------------------
// Exploders: IW3 hides the "exploder" (damaged) state until exploder( n ).
// ---------------------------------------------------------------------------
iw3compat_exploders_init()
{
    level.mw120r_exploders = getentarray( "mw120r_exploder", "targetname" );
    foreach ( piece in level.mw120r_exploders )
        piece hide();
}

iw3compat_exploder( number )
{
    if ( !isdefined( level.mw120r_exploders ) )
        return;

    number = "" + number;
    foreach ( piece in level.mw120r_exploders )
    {
        if ( isdefined( piece.script_exploder ) && ( "" + piece.script_exploder ) == number )
            piece show();
    }

    models = getentarray( "script_model", "classname" );
    foreach ( model in models )
    {
        if ( !isdefined( model.script_exploder ) || ( "" + model.script_exploder ) != number )
            continue;
        if ( isdefined( model.targetname ) && model.targetname == "mw120r_exploder" )
            continue;
        model delete();
    }
}

// ---------------------------------------------------------------------------
// Destructible vehicles (IW3 maps\mp\_destructible_types vehicle_80s_* stages):
// 250 damage -> hood smoke, +200 -> heavy smoke, +100 -> fire that drains
// 300 health at 12 per 0.2s, then the explosion (375 radius, 300..20 damage).
// Melee never damages a vehicle ("no_melee").
// ---------------------------------------------------------------------------
iw3compat_destructible_vehicles()
{
    vehicles = getentarray( "mw120r_destructible", "targetname" );
    foreach ( vehicle in vehicles )
    {
        if ( isdefined( vehicle.destructible_type ) && issubstr( tolower( vehicle.destructible_type ), "vehicle" ) )
            vehicle thread iw3compat_vehicle_think();
    }
}

iw3compat_vehicle_think()
{
    self endon( "death" );
    self setcandamage( 1 );
    self.mw120r_destroyed_model = iw3compat_known_model( self.destructible_type + "_destroyed" );
    self.mw120r_stage = 0;
    self.mw120r_damage = 0;
    self.mw120r_attacker = undefined;
    thresholds = [];
    thresholds[0] = 250;
    thresholds[1] = 450;
    thresholds[2] = 550;
    thresholds[3] = 850;

    for (;;)
    {
        self waittill( "damage", amount, attacker, direction, point, type );
        if ( isdefined( type ) && issubstr( tolower( type ), "melee" ) )
            continue;

        if ( isdefined( attacker ) )
            self.mw120r_attacker = attacker;
        self.mw120r_damage = self.mw120r_damage + amount;

        while ( self.mw120r_stage < 4 && self.mw120r_damage >= thresholds[self.mw120r_stage] )
        {
            self.mw120r_stage++;
            if ( self.mw120r_stage == 1 )
                self thread iw3compat_vehicle_loopfx( "veh_smoke_light", 1, 0.4 );
            else if ( self.mw120r_stage == 2 )
                self thread iw3compat_vehicle_loopfx( "veh_smoke_heavy", 2, 0.4 );
            else if ( self.mw120r_stage == 3 )
            {
                self thread iw3compat_vehicle_loopfx( "veh_fire", 3, 0.4 );
                self thread iw3compat_vehicle_drain();
            }
        }

        if ( self.mw120r_stage >= 4 )
            break;
    }

    self iw3compat_vehicle_explode();
}

iw3compat_vehicle_drain()
{
    self endon( "death" );
    self endon( "mw120r_exploded" );
    while ( self.mw120r_stage == 3 )
    {
        wait 0.2;
        self.mw120r_damage = self.mw120r_damage + 12;
        if ( self.mw120r_damage >= 850 )
        {
            // The damage loop is blocked in waittill; wake it with a zero hit.
            self notify( "damage", 0, self.mw120r_attacker, ( 0, 0, 0 ), self.origin, "MOD_EXPLOSIVE" );
            return;
        }
    }
}

iw3compat_vehicle_hood()
{
    return self.origin + anglestoforward( self.angles ) * 64 + ( 0, 0, 40 );
}

iw3compat_vehicle_loopfx( name, stage, rate )
{
    self endon( "death" );
    self endon( "mw120r_exploded" );
    while ( self.mw120r_stage == stage )
    {
        playfx( level.mw120r_fx[name], self iw3compat_vehicle_hood() );
        wait rate;
    }
}

iw3compat_vehicle_explode()
{
    self notify( "mw120r_exploded" );
    self setcandamage( 0 );
    center = self.origin + ( 0, 0, 32 );
    playfx( level.mw120r_fx["veh_explosion"], center );
    playfx( level.mw120r_fx["veh_glass"], self.origin + ( 0, 0, 48 ) );

    attacker = self.mw120r_attacker;
    if ( isdefined( attacker ) )
        radiusdamage( center, 375, 300, 20, attacker, "MOD_EXPLOSIVE" );
    else
        radiusdamage( center, 375, 300, 20 );

    physicsexplosionsphere( center, 375, 0, 4 );
    earthquake( 0.4, 0.8, center, 1000 );

    if ( isdefined( self.mw120r_destroyed_model ) )
        self setmodel( self.mw120r_destroyed_model );

    self thread iw3compat_vehicle_burn( 12 );
}

iw3compat_vehicle_burn( seconds )
{
    self endon( "death" );
    end = gettime() + seconds * 1000;
    while ( gettime() < end )
    {
        playfx( level.mw120r_fx["veh_fire_linger"], self.origin + ( 0, 0, 24 ) );
        wait 0.5;
    }
}

// ---------------------------------------------------------------------------
// IW3 maps\mp\_destructables: brush/model pieces removed after damage.
// ---------------------------------------------------------------------------
iw3compat_destructables()
{
    pieces = getentarray( "destructable", "targetname" );
    foreach ( piece in pieces )
        piece thread iw3compat_destructable_think();
}

iw3compat_destructable_think()
{
    self endon( "death" );
    self setcandamage( 1 );
    total = 0;
    for (;;)
    {
        self waittill( "damage", amount );
        total = total + amount;
        if ( total >= 40 )
            break;
    }

    playfx( level.mw120r_fx["sparks"], self.origin + ( 0, 0, 6 ) );
    self delete();
}

// ---------------------------------------------------------------------------
// Interactive TVs (IW3 maps\_interactive_objects): the first hit breaks the
// screen with glass and sparks.
// ---------------------------------------------------------------------------
iw3compat_interactive_tvs()
{
    tvs = getentarray( "mw120r_interactive_tv", "targetname" );
    foreach ( tv in tvs )
        tv thread iw3compat_tv_think();
}

iw3compat_tv_think()
{
    self endon( "death" );
    self setcandamage( 1 );
    if ( isdefined( self.model ) )
        self.mw120r_destroyed_model = iw3compat_known_model( self.model + "_d" );
    self waittill( "damage", amount, attacker, direction, point );
    self setcandamage( 0 );
    if ( !isdefined( point ) )
        point = self.origin + ( 0, 0, 16 );

    playfx( level.mw120r_fx["glass_shatter"], point );
    playfx( level.mw120r_fx["sparks"], point );
    if ( isdefined( self.mw120r_destroyed_model ) )
        self setmodel( self.mw120r_destroyed_model );
}
