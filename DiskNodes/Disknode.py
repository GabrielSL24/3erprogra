from flask import Flask, request, jsonify
import os
import base64
import argparse

app = Flask(__name__)

class DiskNode:
    def __init__(self, node_id, storage_path, port, block_size=4096, total_blocks=1000):
        self.node_id = node_id
        self.storage_path = storage_path
        self.port = port
        self.block_size = block_size
        self.total_blocks = total_blocks
        self.used_blocks = 0
        os.makedirs(storage_path, exist_ok=True)

    def validate_block_size(self, data):
        if len(data) > self.block_size:
            raise ValueError(f"Block size exceeds maximum of {self.block_size} bytes")

@app.route('/write_block', methods=['POST'])
def write_block():
    try:
        data = request.json
        block_id = data['block_id']  # Formato esperado: "block_filename_stripe_X" o "parity_filename_stripe_X"
        block_data = data['data']
        is_parity = data.get('is_parity', False)  # Se mantiene para verificación
        
        # Verificar consistencia del nombre
        expected_prefix = "parity_" if is_parity else "block_"
        if not block_id.startswith(expected_prefix):
            raise ValueError(f"Prefijo incorrecto en block_id. Esperado: {expected_prefix}")
        
        # Convertir datos
        if isinstance(block_data, str):
            block_data = base64.b64decode(block_data)
        elif isinstance(block_data, list):
            block_data = bytes(block_data)
        
        # Crear archivo
        filename = f"{block_id}.bin"
        filepath = os.path.join(app.disk_node.storage_path, filename)
        
        with open(filepath, 'wb') as f:
            f.write(block_data)
        
        app.disk_node.used_blocks += 1
        return jsonify({"status": "success", "block_id": block_id})
    
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 400

@app.route('/read_block/<block_name>', methods=['GET'])
def read_block(block_name):
    try:
        filepath = os.path.join(app.disk_node.storage_path, f"{block_name}.bin")
        
        if os.path.exists(filepath):
            with open(filepath, 'rb') as f:
                data = base64.b64encode(f.read()).decode('utf-8')
            
            return jsonify({
                "status": "success",
                "data": data,
                "is_parity": block_name.startswith('parity_')
            })
        
        return jsonify({"status": "error", "message": "Block not found"}), 404
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/status', methods=['GET'])
def status():
    try:
        # Contar solo archivos .bin válidos
        bin_files = [f for f in os.listdir(app.disk_node.storage_path) 
                    if f.endswith('.bin') and os.path.isfile(os.path.join(app.disk_node.storage_path, f))]
        
        return jsonify({
            "node_id": app.disk_node.node_id,
            "port": app.disk_node.port,
            "used_blocks": len(bin_files),
            "total_blocks": app.disk_node.total_blocks,
            "storage_path": app.disk_node.storage_path,
            "status": "active",
            "details": {
                "block_files": sum(1 for f in bin_files if f.startswith('block_')),
                "parity_files": sum(1 for f in bin_files if f.startswith('parity_'))
            }
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/delete_block/<path:block_name>', methods=['DELETE'])
def delete_block(block_name):
    try:
        print(f"[NODE DEBUG] Petición DELETE recibida. Valor crudo de block_name: {block_name}")
        filename = f"{block_name}.bin"
        filepath = os.path.join(app.disk_node.storage_path, filename)
        
        if os.path.exists(filepath):
            os.remove(filepath)
            app.disk_node.used_blocks -= 1
            return jsonify({"status": "success", "block_id": block_name})
        else:
            return jsonify({"status": "error", "message": "Block not found"}), 404
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Servidor DiskNode para sistema RAID distribuido')
    parser.add_argument('--port', type=int, required=True, help='Puerto del servidor')
    parser.add_argument('--node_id', type=int, required=True, help='ID del nodo (1-4)')
    parser.add_argument('--path', type=str, required=True, 
                       help='Ruta de almacenamiento para los bloques')
    args = parser.parse_args()

    # Configurar e iniciar el servidor
    app.disk_node = DiskNode(
        node_id=args.node_id,
        storage_path=args.path,
        port=args.port,
        block_size=4096,       # Tamaño fijo de bloque
        total_blocks=1000      # Capacidad total en bloques
    )
    
    print(f"Iniciando DiskNode {args.node_id} en puerto {args.port}")
    print(f"Almacenamiento en: {args.path}")
    print(f"Tamaño de bloque: 4096 bytes")
    print(f"Capacidad total: {1000*4096/1024/1024:.2f} MB")
    
    app.run(host='0.0.0.0', port=args.port, threaded=True)