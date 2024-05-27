// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"


#define PAGES 512 * 32 // количество страниц, половина всей памяти
#define NODES (2 * PAGES - 1) // количество нодов в buddy дереве
#define DEPTH 15 // глубина buddy дерева

extern char end[];


struct node {
    int state;
    // 0 -- does not exist
    // 1 -- is used
    // 2 -- inner node
    // 3 -- free node

    int id;
    int size; // какому количеству страниц соответствует
    int lvl; // высота: 0 у листьев, DEPTH - 1 у корня 
    struct node* left_child; // левый потомок
    struct node* right_child; // правый потомок
    struct node* prev; // предыдущий node в соответствующем списке свободных нодов (если таковой есть)
    struct node* next; // следующий node в соответствующем списке свободных нодов (если таковой есть)
    struct node* parent; // родитель
    struct node* neighbour; // сосед
    char* memory; // указатель на нужную страницу
};

struct {
    struct spinlock lock;
    struct node nodes[NODES];
    struct node* lists[DEPTH];
    int sizes[DEPTH];
} buddy_metadata;


// добавляем свободный нод в нужный список
void add_free_node(struct node* n) {
    buddy_metadata.sizes[n->lvl]++; // увеличить длину списка
    n->next = buddy_metadata.lists[n->lvl]; // добавляем в начало списка, поэтому тот нод, который был первым, станет вторым
    buddy_metadata.lists[n->lvl] = n; // добавляем в начало
    n->prev = 0; // нет предыдущих
    if (n->next != 0) { // если до этого список был не пуст, то у того нода, который бвл первым, теперь есть предыдущий
        n->next->prev = n;
    }
}

void print_cur_info() {
    int sizes[10], free = 0;
    for (int i = 0; i < 10; ++i) {
        sizes[i] = buddy_metadata.sizes[i];
        free += (buddy_metadata.sizes[i] << i);
    }
    for (int i = 10; i < DEPTH; ++i) {
        sizes[9] += (buddy_metadata.sizes[i] << (i - 9));
        free += (buddy_metadata.sizes[i] << i);
    }
    printf("used = %d, free = %d, sizes: ", PAGES - free, free);
    for (int i = 0; i < 9; ++i) {
        printf("%d, ", sizes[i]);
    }
    printf("%d\n", sizes[9]);
}

void buddy_init() {
    initlock(&buddy_metadata.lock, "buddy_mem");
    for (int idx = 0; idx < DEPTH; ++idx) {
        buddy_metadata.lists[idx] = 0;
        buddy_metadata.sizes[idx] = 0;
    }

    // инициализируем корень
    struct node* current_node = &buddy_metadata.nodes[0];
    current_node->state = 3; // он пустой
    current_node->id = 0;
    current_node->size = PAGES;
    current_node->lvl = DEPTH - 1;
    current_node->left_child = &buddy_metadata.nodes[1];
    current_node->right_child = &buddy_metadata.nodes[2];
    current_node->prev = 0;
    current_node->next = 0;
    current_node->parent = current_node;
    current_node->neighbour = current_node;
    current_node->memory = (char*)PGROUNDUP((uint64)end);
    add_free_node(&buddy_metadata.nodes[0]);

    for (int id = 1; id < NODES; ++id) {
        current_node = &buddy_metadata.nodes[id];
        struct node* parent = &buddy_metadata.nodes[(id - 1) / 2];

        current_node->state = 0; // не существует
        current_node->id = id;
        current_node->lvl = parent->lvl - 1;
        current_node->size = parent->size / 2;
        
        if (id < NODES / 2) {
            current_node->left_child = &buddy_metadata.nodes[2 * id + 1];
            current_node->right_child = &buddy_metadata.nodes[2 * id + 2];
        } else {
            current_node->left_child = 0;
            current_node->right_child = 0;
        }
        
        current_node->prev = 0;
        current_node->next = 0;

        current_node->parent = parent;
        if (id % 2 == 1) {
            current_node->neighbour = &buddy_metadata.nodes[id + 1];
        } else {
            current_node->neighbour = &buddy_metadata.nodes[id - 1];
        }
        
        if (id % 2 == 1) {
            current_node->memory = parent->memory;
        } else {
            current_node->memory = parent->memory + current_node->size * PGSIZE;
        }   
    }
}


void buddy_free(void *pa) {
    if (pa == 0 || ((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP) { 
        panic("buddy_free");
    }

    acquire(&buddy_metadata.lock);

    struct node* current_node = &buddy_metadata.nodes[0];
    while (current_node->state == 2) { // спускаемся от корня к занятому(1) ноду, начинающемуся с pa
        if (current_node->right_child->memory > (char*)pa) {
            current_node = current_node->left_child;
        } else {
            current_node = current_node->right_child;
        }
    }
    
    // если вершину уже освободили, или она не была занятой, или происходит free по адресу где-то в середине
    if (current_node->state != 1 || current_node->memory != (char*)pa) {
        panic("buddy_free");
    }

    if (current_node->id == 0) { // освобождается корень
        add_free_node(current_node);
        current_node->state = 3;
        release(&buddy_metadata.lock);
        return;
    }
    
    while (current_node->id && current_node->neighbour->state == 3) {
        current_node->state = 0;
        current_node->neighbour->state = 0;
        // удаляем соседа cur-a из воответствующего list-а
        if (current_node->neighbour->prev) {
            current_node->neighbour->prev->next = current_node->neighbour->next;
        } else {
            buddy_metadata.lists[current_node->neighbour->lvl] = current_node->neighbour->next;
        }
        if (current_node->neighbour->next) {
            current_node->neighbour->next->prev = current_node->neighbour->prev;
        }
        buddy_metadata.sizes[current_node->neighbour->lvl]--;
        current_node->neighbour->prev = 0;
        current_node->neighbour->next = 0;
        current_node = current_node->parent;
    }

    add_free_node(current_node);
    current_node->state = 3;
    release(&buddy_metadata.lock);
}


void* buddy_alloc(int n) { // количество страниц которые надо аллоцировать
    // проверим, что n -- степень двойки от 1 до 512
    if (n <= 0 || n > 512) {
        return 0;
    }

    int pow2 = 1, lvl = 0;
    while (pow2 < n) {
        pow2 *= 2;
        lvl++;
    }
    
    if (n != pow2) {
        return 0;
    }

    acquire(&buddy_metadata.lock);

    int split_lvl = -1;
    for (int idx = lvl; idx < DEPTH; ++idx) {
        if (buddy_metadata.sizes[idx] > 0) {
            split_lvl = idx;
            break;
        }
    }

    if (split_lvl == -1) {
        printf("cannot find a free node for allocation!\n");
        release(&buddy_metadata.lock);
        return 0;
    }

    struct node* current_node = buddy_metadata.lists[split_lvl];
    
    // удаляем cur из соответствующего list, потому что теперь этот нод станет внутренним
    if (current_node->prev) {
        current_node->prev->next = current_node->next;
    } else {
        buddy_metadata.lists[current_node->lvl] = current_node->next;
    }
    if (current_node->next) {
        current_node->next->prev = current_node->prev;
    }
    buddy_metadata.sizes[current_node->lvl]--;

    current_node->prev = 0;
    current_node->next = 0;

    while (current_node->lvl > lvl) {
        current_node->state = 2; // cur стал внутренним
        add_free_node(current_node->right_child); // появился новый пустой нод
        current_node->right_child->state = 3;
        current_node = current_node->left_child;
    }
    current_node->state = 1; // cur делаем занятым
    
    release(&buddy_metadata.lock);

    return current_node->memory;
}
