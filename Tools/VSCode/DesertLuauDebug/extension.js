// The editor IS the debug adapter (Editor/Source/Editor/LevelEditor/ScriptDebugServer.cpp): this extension only
// tells VS Code where it listens. Start the server in the editor first (palette: Script Debugger > Start DAP
// Server).
const vscode = require( 'vscode' );

function activate( context )
{
    context.subscriptions.push( vscode.debug.registerDebugAdapterDescriptorFactory( 'desert-luau', {
        createDebugAdapterDescriptor( session ) {
            const config = session.configuration;
            return new vscode.DebugAdapterServer( config.port || 21110, config.host || '127.0.0.1' );
        },
    } ) );
}

function deactivate()
{
}

module.exports = {
    activate,
    deactivate
};
