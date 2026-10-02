import os

instructions = []
addr = 0

def add_inst(inst_str):
    global addr
    instructions.append(f'INST 0x{addr:08x} {inst_str}')
    addr += 4

# Init
add_inst('ADDI 2 0 0 100') # x2 = 100 (Outer loop counter)

outer_loop_start = addr

# Loop A (counter x3 = 4)
add_inst('ADDI 3 0 0 4')
loop_a_start = addr
for i in range(9): add_inst('ADDI 1 1 0 1')
add_inst('ADDI 3 3 0 -1')
add_inst(f'BNE 0 3 0 0x{loop_a_start:08x}')

# Loop B (counter x4 = 6)
add_inst('ADDI 4 0 0 6')
loop_b_start = addr
for i in range(9): add_inst('ADDI 1 1 0 1')
add_inst('ADDI 4 4 0 -1')
add_inst(f'BNE 0 4 0 0x{loop_b_start:08x}')

# Loop C (counter x5 = 3)
add_inst('ADDI 5 0 0 3')
loop_c_start = addr
for i in range(9): add_inst('ADDI 1 1 0 1')
add_inst('ADDI 5 5 0 -1')
add_inst(f'BNE 0 5 0 0x{loop_c_start:08x}')

# Outer loop end
add_inst('ADDI 2 2 0 -1')
add_inst(f'BNE 0 2 0 0x{outer_loop_start:08x}')

add_inst('HALT 0 0 0 0')

pisa_content = "PIPEISA 1\nENTRY 0x00000000\n"
pisa_content += '\n'.join(instructions)
pisa_content += '\nEND\n'

os.makedirs('untracked_tests', exist_ok=True)
with open('untracked_tests/complex_workload.pisa', 'w') as f:
    f.write(pisa_content)

print('Generated complex_workload.pisa')
