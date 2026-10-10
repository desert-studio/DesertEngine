-- REFERENCE third/first-person player controller (GP2a). The engine owns the movement model (Character
-- Controller: walking, falling, jump, crouch in cm and cm/s, UE CharacterMovementComponent) and the camera boom
-- (Spring Arm). This script is the pawn's INPUT BINDING, UE's SetupPlayerInputComponent: it reads the Enhanced
-- Input actions of the engine's default context and turns them into intent.
--
-- Setup:
--   * Character Controller + Enhanced Input Player (Mapping Contexts: Engine/Input/IMC_Default) on the player.
--   * A child entity with a Spring Arm, and the Camera as ITS child (third person); or a Camera child at the
--     head (first person).
--   * The visible body: a child entity with the Humanoid skinned mesh (Engine/Meshes/Skinned/Humanoid.skmesh)
--     and an Animation component whose Graph is Engine/Meshes/Skinned/Humanoid_Locomotion.danimgraph. The
--     engine writes Speed (cm/s) and IsFalling into it every frame from the Character Controller
--     (LocomotionSystem); the graph picks Idle / Walk / Run by Speed and Jump while falling.
--   * Optional swimming: World.set("waterLevel", <cm>) in a level script.

Properties = {
    LookSens = 0.0025, -- radians per mouse-pixel
}

function OnStart()
    if not self:has("CharacterController") then
        Log.warn(self:name() .. ": PlayerController needs a Character Controller component")
    end
    Input.lockCursor()
end

function OnUpdate(dt)
    -- IA_Look (Mouse2D, Y negated in IMC_Default): yaw the body, pitch the arm / camera.
    local lx, ly = Input.actionValue("IA_Look")
    self:addYaw(-lx * Properties.LookSens)
    self:addCameraPitch(ly * Properties.LookSens)

    -- IA_Move: x = right, y = forward.
    local mx, my = Input.actionValue("IA_Move")
    self:move(my, mx)

    local swimming = false
    if World.has("waterLevel") then
        local _, y = self:getPosition()
        swimming = y < World.get("waterLevel")
    end
    self:setSwimming(swimming)

    if swimming then
        local vertical = (Input.actionOngoing("IA_Jump") or Input.actionTriggered("IA_Jump")) and 1 or 0
        if Input.actionTriggered("IA_Crouch") then vertical = vertical - 1 end
        self:swim(vertical)
    else
        self:crouch(Input.actionTriggered("IA_Crouch"))
        if Input.actionStarted("IA_Jump") then
            self:jump()
        end
    end
end
